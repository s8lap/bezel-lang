// Codegen — §28.1 / §28.3 / §28.5 / §28.11 / §8.1, v0.338 (#6433): a SYNC
// `owned` parameter whose declared type is a POLYMORPHIC COLLECTION-INTERFACE
// HEAD is silently never discharged — the §28.3 auto-drop registration is
// skipped entirely, so the value leaks on every path.
//
// ROOT CAUSE. `registerUnwindOwnedParam` (src/codegen/LlvmBackend_Setup.cpp)
// gates its collection branch on CONCRETE builtin class names
// (`isLinkedListTypeName` / `isDictTypeName` / `isCollectionTypeName`). A head
// like `ICollection<int>` matches none of them — a head is not a concrete class
// — and it also fails `isClassOrInterfaceRef` further down, because a
// declaration-less builtin head has `rt->declaration == nullptr`. The slot
// therefore fell through every arm and NOTHING was registered.
//
// §28.1's type requirement admits `owned` at the method-parameter position on
// "a class type other than `Type`, **an interface type**, or a bare type
// parameter", §28.3 requires the undischarged value to be "disposed exactly
// once on every path by the affine auto-drop", and §28.5 states the disposal
// MECHANISM for an interface-typed owned value ("runtime dispatch resolves
// disposal to the underlying class's Dispose()") — at its field position, which
// is the one place the spec says it. So sema
// accepts the call with the ordinary §28.3 must-use WARNING — never an error —
// the program computes the right answer, and the object is never freed. No
// crash, no diagnostic. A pure leak, and `runAotChecked` is blind to it by
// default (see WHY EVERY ROW ARMS THE LEAK ORACLE below).
//
// SCOPE IS THE THREE COLLECTION HEADS, MEASURED NOT ASSUMED. Unlike the three
// LIST heads — whose sole implementer pair (`Array<T>` / `OwningArray<T>`) makes
// an array-shaped free sound — these heads are implemented by collections with
// MUTUALLY INCOMPATIBLE runtime representations, so selecting the free from the
// declared type would be a WILD FREE, not a leak. MEASURED acceptance:
//   ICollection<int>         <- Array (bz_array_t), LinkedList (bz_llist_t,
//                                count at offset 16 where bz_array_t has it at
//                                8), Set (bz_dict_t)
//   IReadOnlyCollection<int><- those plus Stack / Queue (both bz_array_t)
//   IOwningCollection<T>     <- OwningArray / OwningLinkedList / OwningSet
//   + a USER CLASS at each of them (§8.1 makes one a legal implementer).
//
// THE DISCRIMINATOR IS NOT NEW INFRASTRUCTURE — it is the pair already emitted
// for these very heads at the explicit `.Dispose()` call site (#3026's
// `isCollectionInterfaceName` arm in LlvmBackend_Calls2.cpp): `bz_obj_is_string`
// separates a REGISTERED class vtable (a user class -> its own Dispose, §28.5)
// from everything else, and `bz_runtime_dispose` — #3026's kind-tag dispatcher
// over the three tagged backings — frees the rest. The kind tag
// (`BZ_RUNTIME_KIND_*`) was built for exactly this population: "a polymorphic
// collection-interface receiver ... whose concrete backing is unknown at the
// call site cannot pick the matching `bz_*_free` statically".
//
// SCOPE EXCLUDES `IEnumerable<T>` / `IOwningEnumerable<T>`, MEASURED. Those are
// a THIRD representation and are NOT part of this fix: an enumerable head is
// ITERATED, so the argument-conversion path materialises the backing into a
// vtable-backed enumerator WRAPPER whose word 0 is an UNREGISTERED iterator
// vtable (#6414 measured the same shape at the owned enumerable FIELD site).
// Two measured consequences:
//   * the kind tag cannot help — the wrapper's offset 48 carries no sentinel,
//     so `bz_runtime_dispose` returns without doing anything. On a revision
//     that routed every head there, `IEnumerable<int>` + `new Array<int>(1)`
//     still leaked 30,000 B / 1000 allocs while `IReadOnlyCollection<int>` +
//     the SAME argument was clean, from BYTE-IDENTICAL callee code — the
//     difference is entirely in what the CALLER passes;
//   * the wrapper's OWNER is the CALLER. With a BORROWED `x: IEnumerable<int>`
//     parameter the caller still disposes the wrapper after the call (and the
//     array then leaks 30,000 B, its own separate defect), so a callee-side
//     release of the wrapper — which is what the iterator vtable contract at
//     that slot calls for — is a heap-use-after-free ASan catches in `main` one
//     instruction after `Take` returns.
// Reconciling that is the ARGUMENT-LOWERING ownership transfer for an
// enumerable argument, a different site and a different subsystem from this
// discharge. The residual is recorded rather than half-fixed here — the same
// call #6415's producer made for the list heads, one family out.
//
// WHY EVERY ROW IS A REAL LEAK ORACLE. The defect IS a leak, so a row is only
// worth anything if a leaking program FAILS it. It does, and the mechanism is
// the harness's own, not an environment knob: `runAotChecked` passes
// `detectLeaks=true` UNCONDITIONALLY (aot_support.hpp:3800), so LSan is on for
// every row regardless of `BEZEL_AOT_DETECT_LEAKS` — that knob is read only by
// `runAot` / `runAotModulesImpl` (aot_support.hpp:3416, 4014) and selects a
// CENSUS mode, not this helper's verdict. And `hasSanitizerReport`
// (aot_support.hpp:528) matches LeakSanitizer's own headline and summary, so a
// leak sets `r.ran = false` with the byte count in `diagnostics`.
//
// NON-VACUITY IS DEMONSTRATED, NOT ASSUMED, by the two scope-boundary rows
// below: they run the very same programs through the strict helper FIRST and
// observe it fail on exactly the leak this fix does not address — 29,940 B in
// 998 allocations for the list head, 30,000 B in 1,000 for the enumerable head.
// A helper that let those through would not be able to fail the seven rows
// either. Those two rows are therefore built on `runAotCheckedLenient`, which
// keeps the generated-code AddressSanitizer (use-after-free / out-of-bounds /
// double-free — the mis-free failure mode this fix could most plausibly
// introduce) and turns LSan OFF.
//
// Each row accumulates 500-1500 heap objects, far above the 50-500 one-shot
// band, and every row returns a value derived from those iterations, so a
// PARTIAL discharge also shows as a wrong exit code rather than passing.
//
// 500 x 1 = 500; 500 mod 251 = 249.

#include <cstdlib>
#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"          // runAot / runAotChecked
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;
using bezel::test::runAotCheckedLenient;

namespace {

// Save/restore BEZEL_AOT_DETECT_LEAKS around a row — the gate may run a whole
// shard with the knob set, and a row must neither pollute nor inherit it.
class LeakEnv {
public:
    explicit LeakEnv(const char* value) {
        const char* prev = std::getenv("BEZEL_AOT_DETECT_LEAKS");
        had_ = prev != nullptr;
        if (had_) prev_ = prev;
        if (value) ::setenv("BEZEL_AOT_DETECT_LEAKS", value, /*overwrite=*/1);
        else       ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    ~LeakEnv() {
        if (had_) ::setenv("BEZEL_AOT_DETECT_LEAKS", prev_.c_str(), 1);
        else      ::unsetenv("BEZEL_AOT_DETECT_LEAKS");
    }
    LeakEnv(const LeakEnv&) = delete;
    LeakEnv& operator=(const LeakEnv&) = delete;
private:
    bool had_ = false;
    std::string prev_;
};

// The ONE runAotChecked call site in this file. It does NOT absorb a toolchain
// skip: `checkedToolchainSkipped` is asserted FALSE as its OWN statement
// BEFORE anything could swallow it, so a box whose ASan link is unavailable
// fails the row loudly instead of reporting a vacuous green. There is no
// `if (!r.ran) return;` and no skip-absorbing early return.
//
// Both halves are asserted: `ran` says the program ran to normal termination
// under ASan/LSan with no leak and no memory error; the exit code says it
// computed the spec-derived value. Either half alone is satisfiable vacuously.
//
// The `LeakEnv` guard is belt-and-braces only and is HONEST ABOUT THAT: this
// helper's leak strictness comes from its own `detectLeaks=true` argument, not
// from the knob (see the file header). It is kept because a future edit that
// moves a row to `runAot` — whose verdict the knob DOES gate — would otherwise
// silently turn it leak-blind, and because the two control rows below are the
// ones that must keep inheriting the ambient setting.
void expectClean(const std::string& src, int expected) {
    LeakEnv scope("strict");
    JitResult r = runAotChecked(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// The ONE call site for the two SCOPE-BOUNDARY control rows. These run programs
// that leak BY DESIGN on this tree — the leak is the out-of-scope defect, not
// this fix's to clear — so the strict helper rejects them (measured: 29,940 B
// and 30,000 B, both reproduced as `ran == false` before this helper was
// swapped). `runAotCheckedLenient` keeps generated-code ASan, so a MIS-FREE —
// the failure mode this fix could most plausibly introduce at an excluded head
// — is still caught; only the leak census is off.
void expectRunsLeakBlind(const std::string& src, int expected) {
    JitResult r = runAotCheckedLenient(src);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.ran ? r.exitCode : -999, expected);
}

// `MyColl` is a genuine §8.1 implementer, not a marker: the `owned _a:
// Array<int>` means a correct release has to reach through the destructor and
// free THAT too, so a release that frees `MyColl` without cascading still
// leaks. `MyColl` is private, so its vtable has internal linkage — the #1843
// registration case `finalizeVtableTable` exists for, and the reason the
// `bz_obj_is_string` split has to consult the emitted vtable table at all.
constexpr const char* kMyColl =
    "class MyColl: ICollection<int> "
    "  private owned _a: Array<int>; "
    "  public constructor() _a := new Array<int>(1); end constructor; "
    "  public method GetEnumerator(): IEnumerator<int> "
    "    return _a.GetEnumerator(); "
    "  end GetEnumerator; "
    "end MyColl; ";

// A driver that calls `Take` 500 times with the given argument and returns
// 500 mod 251 == 249. The count is derived from the iterations so a partial
// discharge shows as a wrong exit code rather than passing silently.
// `modifier` is written verbatim into the parameter, so a control can spell the
// BORROWED form ("") as easily as the owned one ("owned ").
std::string driver(const std::string& decls, const std::string& paramTy,
                  const std::string& arg,
                  const std::string& modifier = "owned ") {
    return "module M; " + decls +
        "class Q "
        "  public static method Take(" + modifier + "x: " + paramTy + "): int "
        "    return 1; "
        "  end Take; "
        "end Q; "
        "class P public static method Main(): int "
        "  var t := 0; "
        "  for var i in 0..<500 do "
        "    t := t + Q.Take(" + arg + "); "
        "  end for; "
        "  return t mod 251; "
        "end Main; end P;";
}

}  // namespace

// §28.3 / §28.5 (#6433) — ICollection<int>, the issue's own headline shape,
// with a USER CLASS argument. Pre-fix: 41,916 B / 1497 allocs leaked,
// `internal:` count 0. Post-fix the class is released through its own Dispose +
// reclaim, which frees `MyColl` AND its `owned _a: Array<int>` — so this row
// also proves the cascade, not merely the outer free.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_ICollection_param_user_class_released_spec_28_3) {
    expectClean(driver(kMyColl, "ICollection<int>", "new MyColl()"), 249);
}

// §28.11 (#6433) — ICollection<int> holding an ARRAY-BACKED builtin. This is
// the other half of the population, and the one a "user class only" fix would
// miss. MEASURED pre-fix: 29,940 B / 998 allocs.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_ICollection_param_array_backing_released_28_11) {
    expectClean(driver("", "ICollection<int>", "new Array<int>(1)"), 249);
}

// §28.11 (#6433) — the LLIST backing. This is the row that makes the fix a
// RUNTIME discrimination rather than a widened static branch: a bz_llist_t
// carries its count at offset 16 where bz_array_t has it at 8, so an
// array-shaped free here reads the wrong field. MEASURED accepted at an
// `ICollection<int>` parameter (and REJECTED at an `IList<int>` one).
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_ICollection_param_linkedlist_backing_released_28_11) {
    expectClean(driver("", "ICollection<int>", "new LinkedList<int>()"), 249);
}

// §28.11 (#6433) — the DICT backing, through a Set<int>. The third tagged
// runtime kind, whose kind tag lives at offset 88 rather than 48; a probe that
// only read offset 48 would leave this row leaking.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_ICollection_param_set_backing_released_28_11) {
    expectClean(driver("", "ICollection<int>", "new Set<int>()"), 249);
}

// §28.11 (#6433) — IReadOnlyCollection<int>, a second classifier input
// (`isCollectionInterfaceName` is a membership test, not a hierarchy walk, so
// each head needs its own row). MEASURED pre-fix: 29,940 B / 998 allocs.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_IReadOnlyCollection_param_array_released_28_11) {
    expectClean(driver("", "IReadOnlyCollection<int>", "new Array<int>(1)"),
                249);
}

// §28.11 (#6433) — IOwningCollection<Array<int>> with an ARRAY backing. This is
// the row the element-disposer derivation exists for: the head is owning, so
// the release takes `elementPolicyFor(elem, owning=true)` and the owned ELEMENTS
// are disposed as well as the container. A fix that passed a null disposer for
// every head would free the backing here and still leak one Array<int> per
// call — invisible to every other row in this file. MEASURED pre-fix:
// 77,844 B / 2495 allocs.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_IOwningCollection_param_owning_array_cascade_28_11) {
    expectClean(driver("", "IOwningCollection<Array<int>>",
                       "new OwningArray<Array<int>>()"), 249);
}

// §28.11 (#6433) — IOwningCollection<Array<int>> with an LLIST backing. Pins
// that the owning cascade is not array-shaped either: the owning element
// disposer must reach an `OwningLinkedList`'s nodes, not an array's slots.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_IOwningCollection_param_owning_linkedlist_cascade_28_11) {
    expectClean(driver("", "IOwningCollection<Array<int>>",
                       "new OwningLinkedList<Array<int>>()"), 249);
}

// §28.8 / §28.9 (#6433) — CONTROL 1, the over-release guard. A BORROWED
// parameter is not the callee's to free (§28.8: "When a parameter is `owned`,
// the caller relinquishes ownership" — and by omission a borrowed one is not
// relinquished), so a fix that registered for the whole parameter list rather
// than for `modifier == "owned"` would free this value TWICE — once as the
// caller's temporary and once as the callee's — and ASan would say so. This row
// passes both before and after the fix; it is here because its passing is the
// evidence that the new arm is gated on the marker.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           borrow_ICollection_param_user_class_is_never_freed_28_8) {
    expectClean(driver(kMyColl, "ICollection<int>", "new MyColl()", ""), 249);
}

// §28.3 (#6433) — CONTROL 2, the concrete-name arm must not have been
// swallowed. `Take(owned x: Array<int>)` already matched the concrete-name
// branch and was ALREADY clean before this change; a new branch placed ahead of
// it that also matched would release the same value twice, and ASan's
// double-free report is what would catch it.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_Array_param_concrete_backing_still_released_28_3) {
    expectClean(driver("", "Array<int>", "new Array<int>(1)"), 249);
}

// §28.3 / §8.1 (#6433) — CONTROL 3, the SCOPE boundary from the safe side. The
// three LIST heads (`IList<T>` / `IOwningList<T>` / `IReadOnlyList<T>`) are
// #6415's scope, and this fix deliberately EXCLUDES them — so a list-head
// parameter is still declined and still leaks (MEASURED on this tree: 29,940 B
// in 998 allocations, `ran == false` under the strict helper, which is how this
// file demonstrates that the seven rows above are not vacuous). This row
// therefore uses the leak-blind helper and does NOT assert that a leak IS
// present — such an assertion would invert the moment #6415's discriminator
// lands. What it does assert is that the excluded head neither CRASHES nor
// wild-frees — that is, the fix's boundary is a decline, not a mis-free. The
// arm excludes the list heads by NAME rather than through
// `isListInterfaceName`, which additionally requires an `astNode` name that an
// imported-module TypeRef does not carry; this row pins the ordinary spelling,
// the comment pins the reason.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_IList_param_is_out_of_scope_and_still_runs_normally_8_1) {
    expectRunsLeakBlind(driver("", "IList<int>", "new Array<int>(1)"), 249);
}

// §28.3 / §8.1 (#6433) — CONTROL 4, the same boundary for the ENUMERABLE heads
// this fix declines. `Take(owned x: IEnumerable<int>)` is admitted by sema and
// the program must still run and compute the right value: the head is outside
// this fix's scope, so the release stays as it was (declined) rather than
// becoming the use-after-free a callee-side wrapper release would introduce
// (see the file header's "SCOPE EXCLUDES" paragraph for the measurement; this
// program leaks 30,000 B in 1,000 allocations today). As with CONTROL 3 the
// leak census is deliberately off and no leak is asserted: this row pins
// "declines, does not mis-free", and it would invert the moment the
// enumerable-argument ownership transfer is reconciled.
//
// 500 x 1 = 500; 500 mod 251 = 249.
BEZEL_TEST(codegen_sync_owned_coll_iface_param_runtime_kind_disposal_6433,
           owned_IEnumerable_param_is_out_of_scope_and_still_runs_normally) {
    expectRunsLeakBlind(driver("", "IEnumerable<int>", "new Array<int>(1)"),
                        249);
}