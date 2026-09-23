// fix #1896 / #1897 — §28.3 "Exception-path discharge" applied to STATE-MACHINE
// frame locals (§30.1 async, §29.2 iterator), spec v0.196. Codegen-observable,
// LeakSanitizer-checked coverage for a leak class that affected every kind of
// state-machine body:
//
//   #1896  an ASYNC method leaked a plain `owned` local when the body threw
//          before the local's normal-path discharge. `r` (a plain `var r :=
//          new Resource()`) is hoisted into the async state-machine frame; when
//          an operation throws after an await, §28.3 "Exception-path discharge"
//          requires `r` to be disposed exactly once during unwinding, exactly as
//          a synchronous method's owned local (and an async method's owned
//          PARAMETER) already were. It leaked instead (16 bytes, in
//          bz$async$*$Resume).
//
//   #1897  the ITERATOR (yield) sibling: `r` hoisted into the iterator state-
//          machine frame leaked when the iterator body threw after a `yield
//          return` (16 bytes, in bz$iter$*$MoveNext). Same root mechanism,
//          distinct backend lowering path.
//
// Root cause (one mechanism, two — really four — lowering sites): the state-
// machine body setups (async Resume, iterator MoveNext, and the async-iterator
// Resume sibling) never keyed unwindIneligibleFn_ to their resume/MoveNext
// function. registerUnwindOwnedLocal gates registration on `currentFn_ ==
// unwindIneligibleFn_` (so a body whose moves/disposes were not analyzed cannot
// register a double-dispose); with the gate keyed to the stale synchronous fn,
// EVERY plain `owned` local in a state-machine body was silently skipped for the
// `unwindOnly` (exception-path) disposal. emitThrowValue / emitPendingCheck
// already emit emitUnwindDisposals on the throw edge — they simply had nothing
// registered to dispose. The fix mirrors the synchronous emitFunctionBody setup
// in each state-machine body (unwindIneligibleFn_ = <resume/MoveNext fn>;
// collectUnwindIneligible(body)). The unwindOnly disposal is skipped on the
// normal edge (§28.3: normal-exit discharge is sema-enforced), so there is no
// double-free on a non-throwing path.
//
// Observation model: class Resource bumps a static net-live counter (ctor +1,
// dtor -1). Each leak leg returns `caught * 10 + Resource._live`, so exit == 10
// proves BOTH that the exception was genuinely thrown and caught (caught == 1)
// AND that `r` was disposed EXACTLY ONCE on the throw edge (live == 0; a leak
// would be 11, a double-free -9 or an ASan/LSan failure). Every leg rides on
// runAotChecked (AOT + ASan/LSan oracle): a leak / double-free / UAF flips
// r.ran to false, so the sanitizer verdict and the destructor-count exit code
// are asserted together. FAIL-WITHOUT/PASS-WITH: before the fix the leak legs
// flag an LSan leak (r.ran == false) AND, without ASan, return 11.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Resource: static net-live counter. Disposed exactly once => net 0; leaked =>
// +1; double-disposed => -1 (and the second reclaim trips ASan under the check).
const char* kResource =
    "class Resource "
    "  public static _live: int := 0; "
    "  public constructor() Resource._live := Resource._live + 1; end constructor; "
    "  destructor Resource._live := Resource._live - 1; end destructor; "
    "end Resource; ";

}  // namespace

// ===========================================================================
// #1896 — ASYNC method: a plain owned frame local live at a throw (after an
// await) is disposed on the exception-unwind edge exactly once (§28.3 / §30.1).
// FAIL-WITHOUT: r leaks in bz$async$Work$Resume — r.ran == false (LSan), and the
// no-ASan value would be 11 (caught=1, live=1). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(exception_unwind_frame_local_1896, async_owned_local_disposed_on_throw_edge_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static async method Work(): Task "
        "    var r := new Resource(); "         // plain owned frame local
        "    await Task.Delay(0); "             // suspend: r is frame-spilled
        "    throw new InvalidOperationException(\"boom\"); "  // §28.3 unwind edge
        "  end Work; "
        "  public static async method Run(): Task<int> "
        "    var caught := 0; "
        "    try "
        "      await Work(); "
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "  // 10 = caught & disposed once
        "  end Run; "
        "  public static async method Main(): Task<int> "
        "    return await Run(); "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 10);   // caught == 1 AND Resource._live == 0
}

// CONTROL (§28.3 / §30.1): a NON-throwing async body whose owned local is
// discharged on the normal path (a `using var` live across an await, disposed on
// fall-off) is disposed EXACTLY ONCE — the new unwind registration must not
// double-free it on the normal edge (the unwindOnly disposal is isUnwind-gated).
// Net Resource._live == 0; ASan/LSan-clean. Passes with and without the fix.
BEZEL_TEST(exception_unwind_frame_local_1896, async_normal_path_owned_local_not_double_freed) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static async method Work(): Task "
        "    using var r := new Resource(); "   // discharged on the normal edge
        "    await Task.Delay(0); "
        "  end Work; "
        "  public static async method Main(): Task<int> "
        "    await Work(); "
        "    return Resource._live; "           // 0 = disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1897 — ITERATOR (yield) method: a plain owned frame local live at a throw
// (after a yield) is disposed on the exception-unwind edge exactly once
// (§28.3 / §29.2). FAIL-WITHOUT: r leaks in bz$iter$Gen$MoveNext — r.ran ==
// false. PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(exception_unwind_frame_local_1897, iterator_owned_local_disposed_on_throw_edge_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Gen(): IEnumerable<int> "
        "    var r := new Resource(); "         // plain owned iterator-frame local
        "    yield return 1; "                  // suspend: r is hoisted into frame
        "    throw new InvalidOperationException(\"boom\"); "  // §28.3 unwind edge
        "  end Gen; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try "
        "      for var x in Gen() do "
        "      end for; "
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "  // 10 = caught & disposed once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// CONTROL (§28.3 / §29.2): a normally-completing iterator whose owned local is a
// `using var` (disposed at the iterator's normal end) is disposed EXACTLY ONCE
// across full consumption — the new unwind registration must not perturb it.
// Two yields consumed => sum 3; net Resource._live == 0; ASan/LSan-clean.
BEZEL_TEST(exception_unwind_frame_local_1897, iterator_normal_completion_owned_local_not_double_freed) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Gen(): IEnumerable<int> "
        "    using var r := new Resource(); "   // disposed on the normal end
        "    yield return 1; "
        "    yield return 2; "
        "  end Gen; "
        "  public static method Main(): int "
        "    var sum := 0; "
        "    for var x in Gen() do "
        "      sum := sum + x; "
        "    end for; "
        "    return sum * 10 + Resource._live; "  // 30 = consumed & disposed once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 30);
}

// ===========================================================================
// #1896/#1897 sibling — ASYNC ITERATOR (async + yield / IAsyncEnumerable): the
// same exception-path-discharge gap in a third state-machine lowering. A plain
// owned frame local live at a throw after a suspend is disposed exactly once
// (§28.3 / §29.2 / §30.1). FAIL-WITHOUT: r leaks in bz$asynciter$Gen$Resume.
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(exception_unwind_frame_local_1896, async_iterator_owned_local_disposed_on_throw_edge_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static async method Gen(): IAsyncEnumerable<int> "
        "    var r := new Resource(); "         // plain owned async-iter frame local
        "    await Task.Delay(0); "
        "    yield return 1; "
        "    throw new InvalidOperationException(\"boom\"); "  // §28.3 unwind edge
        "  end Gen; "
        "  public static async method Main(): Task<int> "
        "    var caught := 0; "
        "    try "
        "      await for var x in Gen() do "
        "      end for; "
        "    catch var e: InvalidOperationException => "
        "      caught := 1; "
        "    end; "
        "    return caught * 10 + Resource._live; "  // 10 = caught & disposed once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}
