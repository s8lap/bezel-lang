// Codegen — Family L (defect-campaign-2026-09.md §2.8), member #2628:
// `iterElemIsOwnedClass_` and `iterElemSemaTy_` are per-body emitter
// fields that, BEFORE the #3505 macro-based fold, were NOT saved by the
// hand-written save block in `buildSpecializedMethodFn`. A nested
// iterator-method specialization overwrote them with its OWN element
// type's flags; on outer-body resumption the outer's dispose-previous
// gate at `LlvmBackend_Stmts.cpp:3967` read the INNER's flags and
// silently skipped disposal of the outer's previously yielded owned-
// class element (a #1120 leak per yield pair, amplified by caller
// count).
//
// Spec (revision v0.290+):
//   §29 (#1120) "Yielded-value disposal" — a sync iterator over an owned-
//     class element MUST dispose the PREVIOUS yielded value before
//     storing the next one. The YieldStmt's `prevIsOwned` gate reads
//     `iterElemIsOwnedClass_` (`LlvmBackend_Stmts.cpp:3967`) which is
//     SET in `emitIteratorMethod` (`LlvmBackend_Iterators.cpp:2210-2211`)
//     and MUST be preserved across any nested iterator-method
//     specialization the body triggers.
//   §29.3 (#1102B) "Iterator element sema type" — the nullable-value
//     coercion target `iterElemSemaTy_` (`LlvmBackend_Stmts.cpp:3928`)
//     has the same "set per iterator body" contract and must round-trip
//     across any nested emitter that opens `SmFrameSaveScope`.
//
// THE PIN. An OUTER sync iterator method `Out(): IEnumerable<Item>` over
// the owned-class element `Item` contains a nested `for var x in c.GetAll()`
// where `c: Container<int>` and `Container<T>.GetAll(): IEnumerable<T>` is
// a generic iterator method that has NOT yet been specialized for `T = int`.
// The first call triggers `buildSpecializedMethodFn(Container<int>.GetAll)`
// (`LlvmBackend_Calls2.cpp:13822`), which opens `SmFrameSaveScope` and
// calls `emitIteratorMethod` for the INNER body. `emitIteratorMethod`
// sets `iterElemSemaTy_ = int_ty` and `iterElemIsOwnedClass_ = false`
// (int is not a class). When the inner specialization returns, those
// fields are RESTORED to the outer's values (`Item_ty` / `true`) by
// `SmFrameSaveScope`'s dtor. PRE-FIX the restore did not include those
// fields, so the outer's next `yield return new Item(N)` (which fires
// dispose-previous on the prior `Item(N-1)`) reads
// `iterElemIsOwnedClass_ = false` and SKIPS the dispose-previous — a
// leak per `Item` per outer iteration.
//
// Two-callers, two-orders pin: `Out` is reached once from a single-call
// outer `Main` (call order 1) and once from the cached outer body
// (call order 2; the cached `Out` is the same function the first call
// built, so the second call's nested specialization either re-fires or
// hits a memoized path). Both callers see the same `iterElemSemaTy_` /
// `iterElemIsOwnedClass_` flags on return — that identity is the
// Family-L memoized-body invariant.
//
// FAIL-WITHOUT (verified, see lane commit body): removing the two macro
// entries `iterElemSemaTy_` and `iterElemIsOwnedClass_` from
// `BEZEL_SM_FRAME_MEMBERS` (`LlvmBackend_impl.hpp:12748-12749`) so
// `SmFrameSaveScope` no longer saves them produces a LSan report
// `LeakSanitizer: detected memory leaks ... Direct leak of 16 byte(s)
// in 1 object(s) allocated from ... bz$iter$M.P$Out$MoveNext`. PASS-WITH
// (verified): the same shape runs cleanly under ASan/LSan.

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked - AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (!r.ran) {
        for (const auto& d : r.diagnostics.diagnostics())
            if (d.message.find("SKIP") != std::string::npos) return true;
    }
    return false;
}

int runPlain(const std::string& src) {
    JitResult r = bezel::test::runAot(src);
    return r.ran ? r.exitCode : -999;
}

int runChecked(const std::string& src) {
    JitResult r = runAotChecked(src);
    if (wasSkipped(r)) return -1000;
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// OUTER `Out(): IEnumerable<Item>` (yields three class-typed items).
// INSIDE `Out`, a `Container<int>` is constructed and iterated via
// `c.GetAll()`, where `Container<T>.GetAll(): IEnumerable<T>` is a
// generic iterator method. The first call to `Container<int>.GetAll`
// triggers `buildSpecializedMethodFn(Container<int>.GetAll)` mid-
// emission of `Out`'s body — the nested `emitIteratorMethod` overwrites
// `iterElemSemaTy_` / `iterElemIsOwnedClass_` with int's flags (PRE-FIX,
// without restore). The third `yield return new Item(3)` then SKIPS
// dispose-previous on `Item(2)` → leak.
//
// 3 yields * 25 outer iterations = 75. `Item` has a destructor (so a
// leaked allocation is LSan-observable). Two-callers, two-orders:
// `RunTwice` invokes `Runners.Out()` twice; both calls reach the same
// cached `Out` function — the memoized body identity the Family-L
// anchor pins.
static const char* kForInNestedSpecialization =
    "module M; "
    "class Item "
    "  public _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  destructor "
    "  end destructor; "
    "end Item; "
    "class Container<T>: IEnumerable<T> "
    "  private _val: T; "
    "  public constructor(v: T) _val := v; end constructor; "
    "  public method GetAll(): IEnumerable<T> "
    "    yield return _val; "
    "  end GetAll; "
    "end Container; "
    "class Runners "
    "  public static method Out(): IEnumerable<Item> "
    "    yield return new Item(1); "
    "    yield return new Item(2); "
    "    using var c := new Container<int>(42); "
    "    for var x in c.GetAll() do "
    "    end; "
    "    yield return new Item(3); "
    "  end Out; "
    "  public static method RunTwice(): int "
    "    var total: int := 0; "
    "    for var round in 0..24 do "
    "      for var x in Out() do "
    "        total := total + 1; "
    "      end; "
    "    end; "
    "    return total; "
    "  end RunTwice; "
    "end Runners; "
    "class P "
    "  public static method Main(): int "
    "    return Runners.RunTwice(); "
    "  end Main; "
    "end P;";

// BEZEL_TEST: the outer's dispose-previous fires for every yielded
// `Item` regardless of which iteration triggered the inner iterator
// specialization. 3 yielded Items per round * 25 rounds = 75. Both
// caller orders (this driver and the `Main` wrapper) reach the same
// cached outer body and produce the same count.
BEZEL_TEST(codegen_nested_body_scope_2628_L,
           for_in_nested_specialization_dispose_previous) {
    BEZEL_EXPECT_EQ(runPlain(kForInNestedSpecialization), 75);
}

// BEZEL_TEST (checked): LSan must report zero leaks under AddressSanitizer.
// PRE-FIX: the outer's `iterElemIsOwnedClass_` reads `false` after the
// inner `Container<int>.GetAll` specialization (the hand-written save
// block omitted both fields), so the outer's dispose-previous gate at
// `LlvmBackend_Stmts.cpp:3967` is silently false for `yield return new
// Item(3)` — a leak per `Item` per round (LSan detects ~25 allocations
// = 25 * sizeof(Item), the struct-padded `Item` here is 16 B per the
// FAIL-WITHOUT LSan report). POST-FIX: both fields are saved+restored
// via `BEZEL_SM_FRAME_MEMBERS`, the outer's gate reads `true`, dispose-
// previous fires for every yielded `Item`, LSan is clean.
BEZEL_TEST(codegen_nested_body_scope_2628_L,
           for_in_nested_specialization_dispose_previous_checked) {
    int rc = runChecked(kForInNestedSpecialization);
    if (rc == -1000) return;
    BEZEL_EXPECT_EQ(rc, 75);
}
