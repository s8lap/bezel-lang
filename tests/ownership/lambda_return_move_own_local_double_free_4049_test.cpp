// Spec-derived regression tests for issue #4049 — codegen return-move on a
// lambda-own fresh local double-frees inside a lambda checked against an
// owned-return target.
//
// §28.3 *Borrow lifetime rules* / §28.8, v0.294 (verbatim): "A `move` is the
// only ownership-transferring read" — so a `return move x;` whose `x` is the
// lambda's own fresh-owned local hands the SAME OBJECT to the caller that the
// lambda body's scope-end auto-drop (the ownership pass flags a plain-`var`
// owned local whose every other exit is undischarged, #1008 Part B) would
// otherwise dispose too. The disposal on the lambda side is null-guarded at
// the actual free site, but the slot is NEVER NULLed by `move x` for a plain
// `var` (the legacy "keep borrow-after-move legal" rule, lowerUnary's case
// (c) comment), so the lambda's scope-end dispose loads the same pointer the
// caller now owns and frees it — measured heap-use-after-free under ASan.
//
// THREE CLOSELY-RELATED SHAPES (only one is broken):
//
//   1. `var items := move x; return items;`  (move at the REBINDING, bare
//      return) — accepted, sound. #4043's accept-twin row.
//   2. `return x;` (no move, bare return of a lambda's own fresh local) —
//      accepted, sound. The lambda body has no scope-end disposal of `x`
//      because `x` is returned bare (handleReturn marks it Disposed).
//   3. `return move x;` (move directly at the return operand) — ACCEPTED but
//      double-frees. The bare-identifier fast-path (handleReturn) is keyed off
//      the AST `IdentifierExpr` shape and does NOT fire for `move x`; instead
//      handleDischarge marks `x` MovedFrom, the dataflow never runs for lambda
//      bodies so the ownership-pass never records the move as a discharge on
//      `x`'s `everDischarged_` set, the nested-scope auto-drop loop (Ownership
//      .cpp:~1117) therefore flags `x` for scope-end disposal and emits the
//      "implicitly disposed at end of scope" warning, AND codegen's `lowerUnary`
//      for `move` only nulls the source slot for `using var`/iter-var/auto-drop-
//      at-scope-end-out-owned/arm-transfer cases — NOT for the plain-`var`
//      `autoDropAtScopeEnd` case the ownership pass now arms.
//
// ROOT CAUSE: codegen's `lowerUnary` (LlvmBackend_Exprs2.cpp) case (e) — the
// plain-`var` `autoDropAtScopeEnd` arm — is the missing null-site. Adding it
// closes the lambda-side double-dispose exactly as `lowerUnary`'s existing
// case (c) (out-owned recipient) and case (d) (arm-transfer, #4016) close
// their twin shapes.
//
// LOOP-AMPLIFIED 500x per [[lsan-one-shot-leak-tests-pass-vacuously]]: a
// single-iteration accept could be vacuously green on either a leak or a
// double-free that happens to balance out, so each row loops the exact issue
// repro and asserts the FINAL `Res.live == 0`. ASan is on (`runAotChecked`)
// so a real double-free aborts before the assertion is reached.
//
// per §28.3, §28.8, v0.294

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot / runAotChecked — AOT(+ASan) oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A destructor-counter Res: ctor ++, dtor --, surfaced through Res.live so an
// ACCEPT test can assert exact-once disposal (0 == no leak, no double-free).
const char* kRes =
    "module M; "
    "class Res "
    "  public static live: int := 0; "
    "  public tag: int; "
    "  public constructor(t: int) this.tag := t; Res.live := Res.live + 1; end constructor; "
    "  destructor Res.live := Res.live - 1; end destructor; "
    "end Res; ";

}  // namespace

// ===========================================================================
// ACCEPT — `return move <lambda-own-fresh-local>` checked against an
// `owned`-returning function-type slot disposes the value EXACTLY once
// (Res.live == 0 at exit, exit code 0, no ASan abort). The ownership pass
// accepts it as a legitimate ownership transfer; codegen must null the source
// slot so the lambda body's scope-end auto-drop (Ownership.cpp's plain-`var`
// `autoDropAtScopeEnd`) doesn't double-free.
//
// FAIL-WITHOUT this fix: heap-use-after-free under ASan (`runAotChecked`
// aborts); the per-iteration double-free can also drive Res.live to ~500 if
// the dispose is "free twice, leak zero" instead of "free twice, leak once"
// — the loop-amplification is what makes either failure mode observable.
// ===========================================================================

// The issue's exact repro: block-bodied lambda, `() -> owned Res` target.
// Loop-amplify 500x per [[lsan-one-shot-leak-tests-pass-vacuously]].
BEZEL_TEST(lambda_return_move_own_local_double_free_4049,
           blockbody_return_move_own_local_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var mk: () -> owned Res := lambda() => do "
        "        var x := new Res(9); "
        "        return move x; "
        "      end; "
        "      do using var c := mk(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// One-level nesting: the same shape inside a lambda nested inside another
// lambda — the issue body notes "Reproduces identically one level deeper".
// Two layers of the same null-site fix must apply, each independently.
BEZEL_TEST(lambda_return_move_own_local_double_free_4049,
           nested_return_move_own_local_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      var mk: () -> owned Res := lambda() => do "
        "        var inner: () -> owned Res := lambda() => do "
        "          var y := new Res(7); "
        "          return move y; "
        "        end; "
        "        do using var d := inner(); end; "
        "        return new Res(8); "
        "      end; "
        "      do using var c := mk(); end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The CALL-ARGUMENT-direct twin — the lambda literal is passed as a call
// argument to a `() -> owned T`-typed parameter (the #4034 collector's
// target), so the `owned` marker flows to checkOneLambdaOwnedReturnDirect-
// Borrow via callArgDirect, not localDeclDirect. Both paths must dispose
// once.
BEZEL_TEST(lambda_return_move_own_local_double_free_4049,
           callarg_direct_return_move_own_local_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Sink(f: () -> owned Res): int "
        "    do using var c := f(); end; "
        "    return Res.live; "
        "  end Sink; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    var total := 0; "
        "    while i < 500 do "
        "      total := total + Sink(lambda() => do "
        "        var x := new Res(9); "
        "        return move x; "
        "      end); "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

// The DIRECT-return sibling — the lambda literal IS the value returned
// directly from a method whose declared return type is `() -> owned Res` (a
// function-returning-function, `Factory(): () -> owned Res`). The
// `expectedFnType` flows through `returnDirect` instead of `localDeclDirect`
// or `callArgDirect` — yet a third driving path that must observe the fix.
BEZEL_TEST(lambda_return_move_own_local_double_free_4049,
           return_direct_move_own_local_disposes_once_spec_28_3) {
    std::string src =
        std::string(kRes) +
        "class C "
        "  public static method Factory(): () -> owned Res "
        "    return lambda() => do "
        "      var x := new Res(9); "
        "      return move x; "
        "    end; "
        "  end Factory; "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 500 do "
        "      do "
        "        var mk := Factory(); "
        "        do using var c := mk(); end; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return Res.live; "
        "  end Main; "
        "end C;";
    JitResult jr = runAotChecked(src);
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 0);
}

