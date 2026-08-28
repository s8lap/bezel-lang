// fix #3505 — §29.1 / §29.3 / §30.6 (v0.283): a nested state-machine emission
// must not RETRACT the enclosing body's suspend/resume frame.
//
// THE CONTRACT. `clearSmFrame()` (LlvmBackend_Iterators.cpp) retracts the whole
// suspend/resume frame, and `StateMachineFrameScope` runs it at every exit from
// a state-machine emitter. That is one half of a contract; the other half is a
// SAVE/RESTORE around every venue that emits a state machine from the MIDDLE of
// another body — `lowerLambda`'s `async` branch (→ `emitAsyncLambda`) and
// `buildSpecializedMethodFn` (→ `emitIteratorMethod` / `emitAsyncMethod` /
// `emitAsyncIteratorMethod`). Each venue had spelled its half out by hand, and
// five members were on the clear list and on none of the save lists:
//
//     smForInSrcFields_  iterElemSemaTy_  iterElemIsOwnedClass_
//     iterCurrentValOwnedField_  iterIsOwningIter_
//
// so a nested emission wiped them for the ENCLOSING body, and everything the
// enclosing body lowered AFTER it behaved as if it were not an iterator at all.
// Both halves are now generated from ONE list (BEZEL_SM_FRAME_MEMBERS in
// LlvmBackend_impl.hpp), which is what makes the contract structural rather than
// a list somebody must remember to maintain.
//
// WHAT THAT COST. `iterIsOwningIter_` is what §29.1's owning head reads at each
// `yield return` to dispose the value the consumer ACCEPTED but never TOOK (the
// #2621 displaced-value dispose). Declaring an `async lambda` in an owning
// iterator's body therefore disarmed that dispose for every LATER yield: the
// abandoned object was overwritten in the frame's `current_val` slot and leaked.
// `iterElemSemaTy_` is what a nullable-value element type is coerced through
// (#1102B), so the same nesting made every later `yield return` in an
// `IEnumerable<T?>` iterator store nothing at all.
//
// ORACLE. The owning rows use runAotChecked (ASan + LeakSanitizer), and on top
// of that every program's observable is a LIVE DESTRUCTOR COUNTER (`R._n`), so
// the assertion is on exactly-once disposal rather than on "did not crash": a
// leak UNDER-counts and a double free BOTH trips ASan and OVER-counts, and the
// two directions are distinguished by the returned value. The nullable row
// asserts the summed element values, which are 0 when the store is skipped.
//
// MEASURED on b43196a (pre-fix) vs the fix, same programs, same harness:
//   owning iterator + nested `async lambda` : 21 + LeakSanitizer "16 byte(s)
//                                             leaked in 1 allocation(s) ...
//                                             in bz$iter$Gen$MoveNext"  ->  31, clean
//   same program with the lambda removed    : 31, clean  ->  31, clean  (control)
//   same program with a SYNC lambda         : 31, clean  ->  31, clean  (control)
//
// The two controls are load-bearing: they establish that the nested ASYNC
// lambda — the venue that runs clearSmFrame — is the whole perturbation, and
// that the sync-lambda branch (which retracts only a subset by hand, and
// deliberately keeps doing so) was never the venue and still is not.
//
// The SECOND venue is pinned too, because the issue named it and it is not the
// same route: `buildSpecializedMethodFn` builds a lazy monomorphization from
// mid-body, so a plain generic call inside an owning iterator retracts the same
// five members with no lambda in the program at all (21 + a leak -> 31, clean).

#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

int runChecked(const std::string& program) {
    auto r = runAotChecked(program);
    return r.ran ? r.exitCode : -999;
}

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A class whose destructor counts live disposals, so every row below asserts
// EXACTLY-ONCE disposal rather than "the program exited".
const std::string kCountR =
    "class R "
    "  public v: int := 0; "
    "  public static _n: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

// An `async` method for the nested lambda to await, so the lambda is a real
// state machine rather than a degenerate one.
const std::string kAsyncP =
    "  public static async method P(x: int): Task<int> "
    "    await Task.Delay(1); "
    "    return x; "
    "  end P; ";

// The §29.1 manual protocol from #2621: TAKE the first value, then ADVANCE
// twice without taking. R(2) is displaced by the R(3) `yield return` and must
// be disposed there; R(3) is disposed at the exhaustion transition (#2616).
// Net: 3 disposals, 1 taken -> R._n * 10 + taken == 31.
const std::string kDriveAndMain =
    "  private static method Drive(): int "
    "    using var source := Gen(); "
    "    using var en := source.GetOwningEnumerator(); "
    "    var taken := 0; "
    "    if en.MoveNext() then "
    "      using var x := en.TakeCurrent(); "
    "      taken := taken + x.v; "
    "    end; "
    "    var junk := 0; "
    "    while en.MoveNext() do junk := junk + 1; end; "
    "    return taken; "
    "  end Drive; "
    "  public static method Main(): int "
    "    var t := Drive(); "
    "    return R._n * 10 + t; "
    "  end Main; end C;";

}  // namespace

// THE ROW. A sync OWNING iterator declares an `async lambda` between two
// `yield return new R(...)`. `emitAsyncLambda` retracts the frame on its way
// out; without the restore the enclosing MoveNext loses `iterIsOwningIter_`,
// the §29.1 displaced-value dispose stops being emitted for every later yield,
// and the abandoned R(2) leaks (21 + a LeakSanitizer report, pre-fix).
BEZEL_TEST(sm_frame_nested_emission_restore_3505,
           owning_iter_async_lambda_between_yields_keeps_displaced_dispose) {
    std::string program = std::string("module M; import System; ") + kCountR +
        "class C " + kAsyncP +
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    var g: (int) -> Task<int> := async lambda(x) => await C.P(x); "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; " + kDriveAndMain;
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// CONTROL 1 — the identical program with the nested lambda REMOVED. Passes on
// both compilers; it is what establishes that the nested emission, and not the
// iterator shape, is the perturbation.
BEZEL_TEST(sm_frame_nested_emission_restore_3505,
           owning_iter_without_nested_lambda_control) {
    std::string program = std::string("module M; import System; ") + kCountR +
        "class C " + kAsyncP +
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; " + kDriveAndMain;
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// CONTROL 2 — the identical program with a SYNC lambda in the same position.
// `lowerLambda`'s sync branch does not run clearSmFrame and retracts only the
// subset it names by hand, so this shape was always correct; pinning it keeps
// the fix from being read as "the sync branch needed the same clear".
BEZEL_TEST(sm_frame_nested_emission_restore_3505,
           owning_iter_with_nested_sync_lambda_control) {
    std::string program = std::string("module M; import System; ") + kCountR +
        "class C " + kAsyncP +
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    var g: (int) -> int := lambda(x) => x + 1; "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; " + kDriveAndMain;
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// THE SECOND VENUE. `buildSpecializedMethodFn` builds a lazy monomorphization
// from the MIDDLE of another body, and the emit*Method helpers it calls each
// open a StateMachineFrameScope — so a generic call inside an owning iterator
// retracts frame members by a different route, with no lambda anywhere. Its
// gap was NOT the same five: this venue already saved smForInSrcFields_, and
// EIGHT were missing rather than five (the four extra are the #3484-follow-up
// cell-frame members). Its hand-written save list covered 20 of the 28 — an
// earlier revision of this comment said 16. Same observable:
// the §29.1 displaced-value dispose stops being emitted for every later yield.
//
//   pre-fix : 21 + LeakSanitizer report      post-fix: 31, clean
BEZEL_TEST(sm_frame_nested_emission_restore_3505,
           owning_iter_generic_specialization_between_yields_keeps_dispose) {
    std::string program = std::string("module M; import System; ") + kCountR +
        "class C "
        "  public static method Ident<T>(v: T): IEnumerable<T> "
        "    yield return v; "
        "  end Ident; "
        "  public static method Gen(): IOwningEnumerable<R> "
        "    yield return new R(1); "
        "    var q := 0; "
        "    for var z in C.Ident<int>(5) do q := q + z; end; "
        "    yield return new R(2); "
        "    yield return new R(3); "
        "  end Gen; " + kDriveAndMain;
    BEZEL_EXPECT_EQ(runChecked(program), 31);
}

// A SECOND member of the same five, on a different consumer: `iterElemSemaTy_`
// is what #1102B coerces a bare `yield return <T>` into an `IEnumerable<T?>`
// element slot with. Retracted by the nested emission, the coercion silently
// no-ops and the `if (v->getType() == iterElemTy_)` store guard stops matching,
// so every later element reads back null and the sum is 0 instead of 12.
BEZEL_TEST(sm_frame_nested_emission_restore_3505,
           nullable_element_iter_after_async_lambda_still_stores) {
    std::string program = std::string("module M; import System; class C ") +
        kAsyncP +
        "  public static method Gen(): IEnumerable<int?> "
        "    var g: (int) -> Task<int> := async lambda(x) => await C.P(x); "
        "    yield return 5; yield return 7; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var s := 0; "
        "    for var x in C.Gen() do "
        "      if x <> null then s := s + x!; end; "
        "    end; "
        "    return s; "
        "  end Main; end C;";
    BEZEL_EXPECT_EQ(run(program), 12);
}
