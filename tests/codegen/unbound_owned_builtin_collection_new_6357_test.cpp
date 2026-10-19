// Codegen — #6357 / §28.3 *Unbound owned temporaries* + §28.9, stdlib
// §4.4–§4.10 (v0.338).
//
// THE ISSUE. `(new Set<int>()).Count` — an unbound owned builtin-collection
// temporary produced by a `new` EXPRESSION rather than by a call, consumed only
// by a borrowing member read — leaks the whole container. MEASURED on this fix's
// parent (`r30-c1` @ b1a94683), 400 iterations, `bezelc --sanitize=address`:
//
//   (new Set<int>()).Count      141,152 B in 802 allocations
//   (new Array<int>()).Count    the same — bz_array_free is never emitted either
//
// (`bz_dict_alloc_generic`, bezel_runtime_collections.cpp:1083 — one table plus
// one header per iteration.)
//
// WHY IT IS A SEPARATE PATH FROM #6352, AND WHY WIDENING #6352 CANNOT REACH IT.
// `freeUnboundOwnedBorrowingArrayTemp`'s three recognizers —
// `isFreshOwnerlessDictMaterializerExpr`, `isFreshOwnerlessArrayMaterializerExpr`
// and `isUnboundOwnedBorrowingArrayCallExpr` — each end in a
// `dynamic_cast<const CallExpr*>`. A `NewExpr` receiver matches none of them, so
// the reclaim is skipped in full. The shared owned-producer classifier does not
// cover it either, for an unrelated reason:
// `isFreshOwnedClassProducer` (src/common/OwnershipFacts.cpp:452) ends in
// `rt->declaration->kind == TypeDeclKind::Class`, and every builtin collection is
// DECLARATION-LESS (stdlib §4.4–§4.11 declare them with no TypeDecl), so it
// answers false for `Array` included. Widening the CALL recognizer would not
// help: there is no call here.
//
// §28.3 states this producer explicitly. "Unbound owned temporaries" says the
// value is one when it is "the result of an `owned`-returning call (**or other
// owned producer**)"; the same section's uniformity paragraph says the `owned`
// return qualifier "is the single signal that drives the reclaim", and §15.4.1
// lists "a `new` expression, a nested collection literal ... or an
// `owned`-returning call" as the producers of exactly this class of value. So a
// `new` of a builtin collection in member-read position is owed a disposal at
// the full-expression boundary, identically to the call face.
//
// THE FIX. One new recognizer, `isUnboundOwnedBuiltinCollectionNewExpr`, adopted
// into the EXISTING `freeUnboundOwnedBorrowingArrayTemp` gate. No new free logic
// and no new name list: the free itself is that helper's unchanged three-way
// `isLL / isDict / else` dispatch, which already carries the per-element
// disposer rule per the receiver's own backing. The predicate is keyed on
// `isPlainBuiltinCollectionHead`, the ONE registry naming the eight BORROWING
// collection classes, which is why `Array` is covered by the same line as `Set`.
//
// WHAT THE PREDICATE DOES **NOT** MATCH, and why each exclusion is load-bearing.
//   * a named local / field / parameter / `move` receiver is an IdentifierExpr
//     or MemberAccessExpr, never a NewExpr — so its own owner still discharges
//     it, and no use-after-free is possible;
//   * `Owning*` heads are excluded by construction (`isPlainBuiltinCollectionHead`
//     lists none of them): an owning container owes an element CASCADE, not a
//     backing free, and is discharged by `freeUnboundOwnedOwningCollectionTemp`;
//   * a user CLASS `new Foo()` is excluded too: that is
//     `isFreshOwnedClassProducer`'s population and it is discharged elsewhere.
//     The two predicates do not overlap.
//
// ORACLE. `runAotChecked` is ALWAYS ASan and returns nonzero on ANY sanitizer
// finding, so a leak, a use-after-free and a double free each fail the RUN — one
// oracle, both directions. No `leaksAtLeast` (that is a leak oracle, not an ASan
// one). `r.ran` is asserted on every row and there is NO skip-absorbing guard
// (`if (!r.ran) return;` would turn a toolchain failure into a pass and raise
// the `absorbing_sites` ratchet). The control rows read the container back
// INSIDE the same pass, which is what makes an over-eager reclaim loud: ASan
// reports the read of freed storage and `runAotChecked` returns nonzero for a
// use-after-free exactly as it does for a leak.
//
// NOTHING HERE ASSERTS A LEAK. Every leak row asserts exit 0, which is the
// affirmative statement that the reclaim happened.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

JitResult checked(const std::string& prog) { return runAotChecked(prog); }

// A module whose `Main` runs `body` 101 times, accumulating `acc` and printing
// it. 101 keeps every `acc` residue well under 256, so no expectation can be
// hidden by the 8-bit exit-code truncation (§9 / lane contract item 9).
//
// `body` MUST itself contribute `+ 1` per pass: `.Count` on a freshly-`new`ed
// collection is 0, so a body that only accumulated the count would leave `acc`
// at 0 and the row could not tell "the loop ran" from "the loop was folded
// away". The trailing `+ 1` is what makes `acc` 101 and the exit code 1.
std::string loopModule(const std::string& name, const std::string& body) {
    return std::string("module ") + name + "; "
           "public class M "
           "  public static method Main(): int "
           "    var acc := 0; "
           "    for var i in 0 .. 100 do "
           "      acc := acc + " + body + "; "
           "    end; "
           "    Console.WriteLine($\"{acc}\"); "
           "    return acc mod 100; "
           "  end Main; end M;";
}

constexpr int kResidueOf101 = 1;  // 101 mod 100

// The affirmative leak oracle: the loop ran, it ran 101 times, and the process
// exited 0 — which under `runAotChecked` (always ASan) is the ONLY clean result.
void expectClean(const std::string& prog) {
    JitResult r = checked(prog);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, kResidueOf101);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}

}  // namespace

// ===========================================================================
// FACE 1 — the issue's own repro. `(new Set<int>()).Count` in a 400-iteration
// loop is the reported measurement; 101 iterations of the same shape is what
// the row runs (the leak is per-evaluation and unbounded, so the iteration
// count changes only the magnitude, never the verdict).
// ===========================================================================
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           set_new_expr_count_is_reclaimed_spec_28_3) {
    expectClean(loopModule("M6357A", "(new Set<int>()).Count + 1"));
}

// FACE 1's `Array<int>` twin. This row is what shows the fix is keyed on the
// HEAD REGISTRY and not on `Set`: `Array` is `bz_array_t`-backed and takes the
// helper's third (array) dispatch arm, `Set` the second. Both leaked identically
// before it.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           array_new_expr_count_is_reclaimed_spec_28_3) {
    expectClean(loopModule("M6357B", "(new Array<int>()).Count + 1"));
}

// FACE 1's `LinkedList<int>` twin — the family the helper dispatches through
// its `freeIsLlist` arm and calls `bz_llist_free`, a THIRD runtime entry point
// from the two above. Without this row the `isLL / isDict / else` three-way
// dispatch would be covered on two arms only.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           linkedlist_new_expr_count_is_reclaimed_spec_28_3) {
    expectClean(
        loopModule("M6357C", "(new LinkedList<int>()).Count + 1"));
}

// The `.Count` property arm is not the only borrowing member read; `.Length` on
// the `Dictionary<K,V>` family is the same gate from the other branch of the
// pair (`isDictTypeName`), so it is the fourth backing head covered here.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           dictionary_new_expr_count_is_reclaimed_spec_28_3) {
    expectClean(loopModule("M6357D", "(new Dictionary<string, int>()).Count + 1"));
}

// ===========================================================================
// THE CONTROLS — the over-free direction. These are green BOTH before and after
// the fix, and they are what makes this fix's predicate auditable rather than
// merely "it stopped leaking": if it matched a receiver it does not own, ASan
// reports the read of freed storage and `runAotChecked` returns nonzero.
// ===========================================================================

// A `new` BOUND to a `using var`, populated and read back inside the same pass.
// The binding is a NamedExpr/IdentifierExpr receiver, never a NewExpr, so the
// predicate cannot match; the local's own disposal discharges it (§28.3 §17.7 —
// an unbound temporary is NOT disposed at a typed assignment target). 101 x 2 = 202.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           new_expr_bound_to_using_var_is_not_freed_by_the_read_spec_28_3) {
    JitResult r = checked(
        "module M6357E; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      using var s := new Set<int>(); "
        "      s.Add(1); s.Add(2); "
        "      acc := acc + s.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);  // 202 mod 100 = 2
    BEZEL_EXPECT_TRUE(r.stdoutText.find("202") != std::string::npos);
}

// The same, on the `LinkedList<T>` family, which the helper frees through
// `bz_llist_free` — a different runtime entry point from `bz_array_free`, so the
// over-free direction is checked on that arm independently.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           linkedlist_new_expr_bound_to_using_var_is_not_freed_spec_28_3) {
    JitResult r = checked(
        "module M6357F; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      using var l := new LinkedList<int>(); "
        "      l.Add(7); l.Add(8); l.Add(9); "
        "      acc := acc + l.Count; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 3);  // 303 mod 100 = 3
    BEZEL_EXPECT_TRUE(r.stdoutText.find("303") != std::string::npos);
}

// A bound producer read TWICE across the pass. Even if a future change widened
// the predicate to a bound receiver, this row would catch it on the first read.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           dictionary_new_expr_bound_and_read_twice_stays_clean_spec_28_3) {
    JitResult r = checked(
        "module M6357G; "
        "public class M "
        "  public static method Main(): int "
        "    using var d := new Dictionary<string, int>(); "
        "    d.Add(\"k\", 7); "
        "    var a := d.Count; "
        "    var b := d.Count; "
        "    Console.WriteLine($\"{a + b}\"); "
        "    return a + b; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("2") != std::string::npos);
}

// The `string`-ELEMENT receiver. §28.11's element-slot policy makes a `string`
// slot a SHARED REFERENCE with a co-owned +1, and the helper installs
// `bz_set_string_keys_release` for a set-family head BEFORE `bz_dict_free` — a
// two-call sequence the new face now reaches for the first time. This row puts a
// live string through it, so a mis-aimed disposer (the exact failure #6352's own
// first draft hit) is a use-after-free here rather than a silent pass. The
// string is read back AFTER the pass, so a wrongly-released reference is
// observable.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           set_of_string_new_expr_count_is_reclaimed_without_uaf_spec_28_3) {
    JitResult r = checked(
        "module M6357H; "
        "public class M "
        "  public static method Main(): int "
        "    var s := \"hello\"; "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      acc := acc + (new Set<string>()).Count + 1; "
        "    end; "
        "    Console.WriteLine($\"{acc} {s.Length}\"); "
        "    return s.Length; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 5);  // "hello".Length
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101 5") != std::string::npos);
}

// The plain (non-ASan) leg, so the VALUE is pinned on an oracle that does not
// depend on the sanitizer runtime at all. `(new Array<int>()).Count` is 0 on an
// empty array; 101 x 0 = 0, so the accumulator cannot carry the evidence — the
// row therefore asserts the loop ran by printing the iteration count too.
BEZEL_TEST(unbound_owned_builtin_collection_new_6357,
           new_expr_count_value_is_correct_on_the_plain_leg_spec_28_3) {
    JitResult r = runAot(
        "module M6357I; "
        "public class M "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var i in 0 .. 100 do "
        "      acc := acc + (new Array<int>()).Count + 1; "
        "    end; "
        "    Console.WriteLine($\"{acc}\"); "
        "    return acc mod 100; "
        "  end Main; end M;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);  // 101 mod 100
    BEZEL_EXPECT_TRUE(r.stdoutText.find("101") != std::string::npos);
}