// fix #9053 (§28.3 K3.1, §28.11, §14.2, §14.4, v0.359): a store into an
// OWNING-COLLECTION slot whose ELEMENT TYPE is a type parameter was not
// recognised as the consuming store it is, so the `owned` parameter it stores
// was left undischarged — and the §28.3 must-use lint drew a FALSE warning on
// a program whose store is exactly the spec's K3.1 row.
//
// THE ISSUE. `_items[key] := item;` with `owned item: T` into an
// `OwningDictionary<K, T>` field inside `Repo<K, T> where T: class`:
//
//   warning: owned parameter `item` is implicitly disposed at the end of the
//   method (§28.3 must-use / auto-drop); `move` / `return` / `.Dispose()` it, …
//
// while the NON-GENERIC twin (`OwningDictionary<int, User>`) is silent.
//
// THE SPEC. §28.3's consume table, row K3.1 (v0.359):
//
//   | K3.1 | store into an `owned` field or owning-collection slot, including
//   through a `set(owned)` accessor (§23.3) | none — the declared slot
//   announces (marker-free) | **moved-from** — a later read is an error until
//   reassigned | frozen (K5.1) | none here — the new holder's |
//
// So the store CONSUMES `item` (marker-free — the declared slot announces), and
// the compiler must not ALSO report it as implicitly disposed.
//
// THE ROOT CAUSE. `MethodOwnership::indexerSetterTakesOwned`
// (src/ownership/Ownership_Checks.cpp) decided the owning slot's ownership by
// the slot's ELEMENT TYPE: a `Named` TypeRef with a DECLARED class/interface.
// A type parameter resolves to a `Named` TypeRef with a NULL declaration — the
// shape §14.1 / #6530 records as "a declaration-less single-part Named TypeRef"
// — so the check missed it, the K3.1 store was not modelled as consuming, and
// the owned parameter kept its §28.3 obligation.
//
// The element CATEGORY (§28.11 / §3.6 rule 2) settles the question:
// `OwningArray<T>` (stdlib §4.5) and `OwningDictionary<K, V>` (stdlib §4.9)
// declare `where T: class` / `where V: class`, and the compiler rejects a
// value-typed element at the instantiation site (§14.4's owning-element
// restriction) — so EVERY admissible instantiation has a class/interface
// element and the slot takes owned. `OwningBuffer<T>` (stdlib §4.14) is the
// RECORDED EXCEPTION (it admits value-typed elements with plain value slots),
// so its slot takes owned only when the parameter is class-forced.
//
// THE TWO SYMPTOMS ARE ONE DEFECT — and the second one is a SOUNDNESS hole,
// not only a false warning. §28.11 also says "Indexer set `coll[i] := value`
// requires `value` to be owned", and that rejection rode on the same
// predicate. On the base, a BORROW stored into a GENERIC owning slot was
// therefore silently accepted — and double-freed at run time (measured: an
// AddressSanitizer heap-use-after-free in `bz$elemDispose` — the dictionary's
// element disposer reading a block the holder had already freed). The
// NON-GENERIC twin rejects the same program with "a borrow cannot be passed
// where an `owned` value is required" (§28.8).
//
// ROW 1 — the K3.1 post-state: a later READ of the stored parameter is an
// error (moved-from). RED on the base: the base accepts the read (with the
// false warning), the fixed compiler rejects it.
// ROW 2 — §28.11's owned-slot rule: a BORROW into the same generic owning
// slot is rejected, exactly as in the non-generic twin. RED on the base: the
// base accepts it and double-frees at run time.
// ROW 3 — the CONTROL: the issue's own program, 200 iterations, disposes every
// object EXACTLY ONCE and leaks nothing. GREEN on the base by construction
// (the base's run-time disposal is already correct — the issue is a DIAGNOSTIC
// defect) and RED under a plausible wrong fix: a fix that marks the parameter
// moved-from in the ownership pass but leaves codegen's transferred-slot
// nulling (`nullTransferredOwnedLocalSlot`) off double-frees the element, and
// this row's ASan/LSan leg is what catches that.

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"        // runAot / runAotChecked — the AOT oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// Every diagnostic the harness produced, concatenated — the sanitizer report
// the AOT harness embeds in the diagnostic message when ASan / LSan fires.
std::string diagnosticText(const JitResult& r) {
    std::string out;
    for (const auto& d : r.diagnostics.diagnostics()) {
        out += d.message;
        out += '\n';
    }
    return out;
}

// LeakSanitizer counted nothing: 0 leaked allocations, 0 leaked bytes. LSan
// prints a SUMMARY line only when it finds a leak, so "no summary" is 0.
void assertNoLeak(const JitResult& r) {
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    const std::string text = diagnosticText(r);
    unsigned long long leakedBytes = 0, leakedAllocs = 0;
    const bool sawSummary = bezel::test::aot_detail::parseLeakSummary(
        text, leakedBytes, leakedAllocs);
    if (sawSummary)
        std::cout << "    [ LEAK COUNT ] " << leakedAllocs
                  << " allocation(s) / " << leakedBytes
                  << " byte(s) leaked (expected 0 / 0)\n";
    BEZEL_EXPECT_TRUE(sawSummary == false);
    BEZEL_EXPECT_TRUE(text.find("ERROR: LeakSanitizer") == std::string::npos);
    BEZEL_EXPECT_TRUE(text.find("ERROR: AddressSanitizer") == std::string::npos);
}

// True iff an ERROR-severity diagnostic contains `needle`. Scoped to ERRORs on
// purpose: `runAot` fills `diagnostics` ONLY on a reject (issue #3138), so a
// Warning emitted by a program that COMPILES is invisible here — the
// warning-absence half of #9053 is pinned in the lit track
// (tests/lit/generic_owning_slot_no_autodrop_warning_9053.bzl), never here.
bool hasError(const JitResult& r, const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

// The ISSUE'S REPRO, verbatim in shape, plus the later READ of the stored
// parameter that K3.1's "moved-from — a later read is an error until
// reassigned" post-state demands.
//
// The read is spelled `if item = null then` because `T` is a bare type
// parameter: §3.6 rule 2 exposes only the members its constraints establish,
// so `item.Id` is not nameable on `T` — but the READ of `item` itself is
// exactly the use K3.1's post-state governs.
const char* kReadAfterStore =
    "module M; "
    "public class User(id: int) "
    "  public readonly property Id: int := id; "
    "  destructor Console.WriteLine($\"close {Id}\"); end destructor; "
    "end User; "
    "public class Repo<K, T> where K: hashable where T: class "
    "  private owned _items: OwningDictionary<K, T> := "
    "new OwningDictionary<K, T>(); "
    "  public method Add(key: K, owned item: T) "
    "    _items[key] := item; "
    "    if item = null then "
    "      Console.WriteLine(\"null\"); "
    "    end if; "
    "  end Add; "
    "  public property Count: int => _items.Count; "
    "end Repo; "
    "public class Program "
    "  public static method Main() "
    "    using var r := new Repo<int, User>(); "
    "    r.Add(1, new User(1)); "
    "    Console.WriteLine(r.Count); "
    "  end Main; "
    "end Program; ";

// The same store, with a BORROW as the right-hand side: `h.U` is a read of an
// `owned` field, and §28.3 makes a bare owned-field read a BORROW. §28.11's
// "Indexer set `coll[i] := value` requires `value` to be owned" rejects it —
// the non-generic twin does exactly that.
const char* kBorrowIntoSlot =
    "module M; "
    "public class User(id: int) "
    "  public readonly property Id: int := id; "
    "  destructor Console.WriteLine($\"close {Id}\"); end destructor; "
    "end User; "
    "public class Holder<T> where T: class "
    "  public owned U: T; "
    "  public constructor(u: owned T) this.U := u; end constructor; "
    "end Holder; "
    "public class Repo<K, T> where K: hashable where T: class "
    "  private owned _items: OwningDictionary<K, T> := "
    "new OwningDictionary<K, T>(); "
    "  public method Put(key: K, h: Holder<T>) "
    "    _items[key] := h.U; "
    "  end Put; "
    "end Repo; "
    "public class Program "
    "  public static method Main() "
    "    using var r := new Repo<int, User>(); "
    "    using var h := new Holder<User>(new User(1)); "
    "    r.Put(1, h); "
    "    Console.WriteLine(\"put\"); "
    "  end Main; "
    "end Program; ";

// The issue's program again, looped 200 times with a COUNTING destructor, so
// the oracle is an EXACT disposal count rather than "it did not crash": a
// missed disposal shows as a count below 200 and a leak; a doubled one shows
// as a double free (ASan) — the §28.6 exactly-once rule.
std::string loopProgram(int iterations) {
    return std::string(
        "module M; "
        "public class User(id: int) "
        "  public static disposed: int := 0; "
        "  public readonly property Id: int := id; "
        "  destructor disposed := disposed + 1; end destructor; "
        "end User; "
        "public class Repo<K, T> where K: hashable where T: class "
        "  private owned _items: OwningDictionary<K, T> := "
        "new OwningDictionary<K, T>(); "
        "  public method Add(key: K, owned item: T) "
        "    _items[key] := item; "
        "  end Add; "
        "  public property Count: int => _items.Count; "
        "end Repo; "
        "public class Program "
        "  public static method Main() "
        "    var i: int := 0; "
        "    while i < ") +
        std::to_string(iterations) +
        " do "
        "      using var r := new Repo<int, User>(); "
        "      r.Add(i, new User(i)); "
        "      i := i + 1; "
        "    end while; "
        "    Console.WriteLine(User.disposed); "
        "  end Main; "
        "end Program; ";
}

}  // namespace

// =============================================================================
// ROW 1 — §28.3 K3.1's POST-STATE: the store consumes the parameter, so a
// later READ of it is an error (moved-from until reassigned).
//
// RED on the base: the base does not model the generic owning slot as a
// consuming store, so the read is accepted (with the issue's false warning)
// and the program COMPILES — `r.ran` is true and no error is reported.
// =============================================================================
BEZEL_TEST(generic_owning_slot_consumes_owned_param_9053,
           read_after_generic_owning_slot_store_is_moved_from) {
    auto r = runAot(kReadAfterStore);
    // The program must be REJECTED: the read is a use of a moved-from value.
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r, "owned value in `item` was already moved out at this point (§28.3)"));
    if (r.ran)
        std::cout << "    [ BASE BEHAVIOUR ] the generic owning-slot store did NOT "
                     "consume `item`: the later read compiled\n";
}

// =============================================================================
// ROW 2 — §28.11's OWNED-SLOT rule, reached through the same predicate: a
// BORROW cannot be stored into an owning-collection slot.
//
// RED on the base: the base accepts the program and double-frees at run time
// (measured on the base under ASan: "heap-use-after-free … bz$elemDispose",
// the dictionary's element disposer reading a block the holder had already
// freed). The fixed compiler rejects it at compile time, exactly as the
// non-generic twin does.
// =============================================================================
BEZEL_TEST(generic_owning_slot_consumes_owned_param_9053,
           borrow_into_generic_owning_slot_is_rejected) {
    auto r = runAot(kBorrowIntoSlot);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(hasError(
        r,
        "a borrow cannot be passed where an `owned` value is required"));
    if (r.ran)
        std::cout << "    [ BASE BEHAVIOUR ] a borrow was accepted into the "
                     "generic owning slot (double free at run time)\n";
}

// =============================================================================
// ROW 3 — the CONTROL: the issue's own program, 200 iterations, disposes every
// object EXACTLY ONCE and leaks nothing.
//
// GREEN on the base by construction (the base's run-time disposal is already
// correct — #9053 is a DIAGNOSTIC defect) and RED under a plausible wrong fix:
// a fix that marks the parameter moved-from in the ownership pass but leaves
// codegen's transferred-slot nulling off double-frees the element, and this
// row's ASan/LSan leg is what catches that. This is the "assertNoLeak over a
// loop" check the lane contract admits as a boundary control (a corpus case
// cannot express it).
// =============================================================================
BEZEL_TEST(generic_owning_slot_consumes_owned_param_9053,
           generic_owning_slot_store_disposes_each_object_once) {
    constexpr int kIterations = 200;
    auto r = runAotChecked(loopProgram(kIterations));
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // One disposal per constructed object — no missed one, no second one.
    BEZEL_EXPECT_EQ(r.stdoutText, std::to_string(kIterations) + "\n");
    assertNoLeak(r);
}
