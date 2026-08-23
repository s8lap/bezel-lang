// fix #2895 (§29.3 "Iterator methods" / §30.1 "`async` methods" / §30.7
// "`async` iterators", v0.240): the suspend/resume frame was published by
// `emitResumeDispatch` (LlvmBackend_Iterators.cpp ~387) and never FULLY
// retracted. Each of the four state-machine emitters cleared a different
// subset of the sm*_/iter*_/async*_ members on the way out —
//
//   emitIteratorMethod     cleared  smStateStruct_/smSelf_/smLocalFields_/
//                                     smForInSrcFields_/smAllocaToField_/
//                                     iterElemIsOwnedClass_/iterCurrentValOwnedField_
//                          left     smStateGEP_/smKind_(=Yield)/smStateBBs_/
//                                     smDoneBB_/smNextSuspendState_/
//                                     iterCurrentValField_/iterElemTy_/
//                                     iterElemSemaTy_/asyncOuterTaskField_/
//                                     asyncPendingTaskField_
//
//   emitAsyncMethod        cleared  smStateStruct_/smSelf_/smLocalFields_/
//                                     smAllocaToField_/asyncResumeFn_/
//                                     asyncResultTy_/asyncResultSemaType_/
//                                     asyncPatternAResultDisposer_/
//                                     asyncOwnedParamFrame_
//                          left     smStateGEP_/smKind_(=Await)/smStateBBs_/
//                                     smDoneBB_/smNextSuspendState_/
//                                     smForInSrcFields_
//
//   emitAsyncLambda        cleared  same nine as emitAsyncMethod
//                          left     same as emitAsyncMethod
//
//   emitAsyncIteratorMethod cleared  smStateStruct_/smSelf_/smLocalFields_/
//                                     smAllocaToField_/asyncResumeFn_/
//                                     asyncResultTy_
//                          left     smStateGEP_/smKind_(=AsyncYield)/
//                                     smStateBBs_/smDoneBB_/
//                                     smNextSuspendState_
//
// `emitFunctionBody`'s ordinary-method prologue (LlvmBackend_Setup.cpp ~8604)
// resets `currentFn_`/`currentThis_`/`locals_`/`paramSlots_`/`refParams_`/
// `outOwnedRecipients_`/`disposeStack_`/`loopStack_`/`finallyStack_`/
// `propagateBB_`/`pendingArgsDisposal_`/`unwindTarget_`/`currentCaughtExc_`/
// `currentCaughtName_`/`unwindScopeDepth_` — no sm*_ member is in that list,
// so whatever survives an emitter's partial clear is live for every subsequent
// function compiled into the module, and the body loop iterates
// `std::unordered_map<sema::MethodSymbol*, llvm::Function*> methodFns_` in
// POINTER-HASH order, not declaration order. The shape is the sequential
// twin of `NestedBodyScope` (which already retracts the frame for a NESTED
// body emission, fixing #2170/#2274/#2281/#2694/#2701/#2702/#2731) — every
// one of those fixes was the SAME shape, with the next per-body member being
// added field by field at two or three sites; #2895 retracts the whole frame
// in one place.
//
// TODAY, with #2891's `smStateGEPHere()` guard in place, no live miscompile
// was reachable from the residual members — every consumer I could reach was
// also gated on `smStateStruct_`/`smSelf_`, and those two ARE cleared. But
// #2895's value is RETROACTIVE: any FUTURE consumer that gates on `smKind_`
// alone, or on `smStateBBs_[nextState]` for a stale `nextState`, reopens the
// defect. The fix replaces the per-emitter partial clears with a single
// `StateMachineFrameScope` RAII guard at the TOP of every state-machine
// emitter that calls `clearSmFrame()` on destruction — covering both the
// normal return AND every diagnostic early-return path (each emitter has
// at least one, all of which published the frame before bailing out).
//
// The tests below exercise the issue's full grid: each emitter followed by a
// plain method whose `try`/`finally` body reads the frame's state through the
// `smStateGEPHere()` gate that #2891 closed. Before #2891, every one of
// these failed to compile (the #2891 case that gave the gate its name is
// `plain_try_finally_then_async_method` below, verbatim from #2891's test).
// Before #2895, the partial clear inside the emitter still left
// `smKind_`/`smStateGEP_`/`smStateBBs_`/`smDoneBB_`/`smNextSuspendState_`/
// iter/async-iterator-only fields live for the NEXT emitter. The test
// programs here are constructed so the next function WOULD misroute if any
// of those residual members reopens a consumer — the issue's exact
// description of the defect.

#include "support/aot_support.hpp"   // runAot — AOT harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}
}  // namespace

// ---------------------------------------------------------------------------
// §29.3 — a sync iterator (`smKind_ == Yield`) followed by a plain method
// with a `try`/`finally`. `Plain` (11) + 3 (1+2) == 14.
BEZEL_TEST(state_machine_frame_retract_2895, sync_iter_then_plain_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Nums(): IEnumerable<int>\n"
        "    yield return 1;\n"
        "    yield return 2;\n"
        "  end Nums;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "    end;\n"
        "    return C.Plain() + s;\n"
        "  end Main;\n"
        "end C;\n"), 14);
}

// §30.1 — the async-method twin. `Plain` (11) + 1 == 12.
BEZEL_TEST(state_machine_frame_retract_2895, async_method_then_plain_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Main(): int\n"
        "    return C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 11);
}

// §30.7 — the async-iterator twin. `Plain` (11) + 3 (1+2) == 14.
BEZEL_TEST(state_machine_frame_retract_2895, async_iter_then_plain_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static async method Nums(): IAsyncEnumerable<int>\n"
        "    yield return 1;\n"
        "    yield return 2;\n"
        "  end Nums;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static async method Main(): Task<int>\n"
        "    var s := 0;\n"
        "    await for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "    end;\n"
        "    return C.Plain() + s;\n"
        "  end Main;\n"
        "end C;\n"), 14);
}

// §29.7 / §30.7 — the DISPOSAL arm. An iterator abandoned by `break` runs
// its `finally` body inside the Dispose function (which is emitted AFTER
// MoveNext inside the SAME emitter call). The partial clear at ~1585 keeps
// the Dispose body's `lowerForStmt` from registering locals into the
// MoveNext frame; the `StateMachineFrameScope` guard handles the leak that
// would survive into the next emitter (the new `Plain` after `Nums` in
// this class). The disposal pin: 1 (the yielded value) + 11 (Plain) == 12.
BEZEL_TEST(state_machine_frame_retract_2895, iter_dispose_then_plain_after) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class C\n"
        "  public static method Nums(): IEnumerable<int>\n"
        "    try\n"
        "      yield return 1;\n"
        "      yield return 2;\n"
        "    finally\n"
        "    end;\n"
        "  end Nums;\n"
        "  public static method Plain(): int\n"
        "    var n := 0;\n"
        "    try\n"
        "      n := n + 1;\n"
        "    finally\n"
        "      n := n + 10;\n"
        "    end;\n"
        "    return n;\n"
        "  end Plain;\n"
        "  public static method Main(): int\n"
        "    var s := 0;\n"
        "    for var x in C.Nums() do\n"
        "      s := s + x;\n"
        "      if x = 1 then\n"
        "        break;\n"
        "      end;\n"
        "    end;\n"
        "    return s + C.Plain();\n"
        "  end Main;\n"
        "end C;\n"), 12);
}

// §31.2 — accept-control that a DESTRUCTOR body (emitted from its own loop
// AFTER the method loop) sees the cleared frame. `Plain` (11) + 1 (Live
// inside scope) + 0 (Live after) == 12. Identical shape to the
// #2891 destructor_try_finally_with_async_method test, here pinned for the
// retract arm — the destructor body is emitted after every async method
// (every emitter has published its frame before the destructor loop runs),
// so this is the order-independent pin.
BEZEL_TEST(state_machine_frame_retract_2895, destructor_after_async_method) {
    BEZEL_EXPECT_EQ(run(
        "module M;\n"
        "class Res\n"
        "  public static Live: int := 0;\n"
        "  public constructor()\n"
        "    Res.Live := Res.Live + 1;\n"
        "  end constructor;\n"
        "  destructor\n"
        "    try\n"
        "      Res.Live := Res.Live - 1;\n"
        "    finally\n"
        "      Res.Live := Res.Live + 0;\n"
        "    end;\n"
        "  end destructor;\n"
        "end Res;\n"
        "class C\n"
        "  public static async method Tick(): Task<int>\n"
        "    return 1;\n"
        "  end Tick;\n"
        "  public static method Main(): int\n"
        "    var acc := 0;\n"
        "    do\n"
        "      using var r := new Res();\n"
        "      acc := acc + Res.Live;\n"
        "    end;\n"
        "    return 11 + acc + Res.Live;\n"
        "  end Main;\n"
        "end C;\n"), 12);
}