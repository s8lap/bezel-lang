// fix issue 4193 (stdlib §8.1/§23.2-§23.3, v0.298): ACCEPT control. A bare
// identifier that resolves to a COMPUTED or BLOCK-FORM property getter
// (`property Sw: Stopwatch get => Stopwatch.StartNew(); end;`) is NOT "one
// non-side-effecting load" — reading it bare evaluates the getter body
// against `this` (`lowerExprInner`'s own `IdentifierExpr` handling,
// LlvmBackend_Exprs.cpp, `computedGetterOf`/`blockGetterOf`), which can emit
// a real side-effecting call before `emittedSideEffectSince` is asked
// whether the MEMBER itself dispatched.
//
// This file pins that the fix (`Emitter::postGetterFootprint_`,
// LlvmBackend_impl.hpp / LlvmBackend_Exprs.cpp / LlvmBackend_Calls2.cpp) does
// NOT reintroduce the false positive a first, broader attempt at it
// measurably caused: extending Shape 1's `isConcreteType` check (the OTHER
// diagnostic this fix touches) to treat every declaration-less builtin as
// concrete made `SwExpr.Start();` spuriously warn "cannot dispatch member
// 'Start'" — the exact class of false positive fix #4171 exists to stop, just
// through a getter-backed receiver instead of the `Stopwatch.StartNew()`-typed
// LOCAL receiver #4171 itself covers. The shipped fix instead narrows the
// footprint anchor to right after the getter finishes, so it never has to
// widen `isConcreteType` at all — see the sibling reject files
// (stopwatch_getter_receiver_bogus_expr_getter_4193.bzl and
// _bogus_block_getter_4193.bzl) for the diagnostic this fix RESTORES.
//
// THIS FILE IS THE DIAGNOSTIC ORACLE for the accept side, per fix #2963's own
// convention: the AOT gtest harness only populates `JitResult::diagnostics`
// on a compile REJECT, so an "assert no warning" written there passes
// VACUOUSLY on any compiler, fixed or not. The runtime half — that
// `Start`/`Stop`/`Reset` genuinely dispatch — is pinned separately in
// tests/codegen/builtin_receiver_unlowered_member_2935_test.cpp
// (`real_stopwatch_api_via_bare_expr_getter_receiver_still_works_4193` and
// its block-form sibling).
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --allow-empty %s

module M;

import System.Diagnostics;

class C
  // With the #5351 fix, Stopwatch.StartNew() returns owned, which cannot be
  // returned from a getter (§28.6). Store in an owned field and return a borrow.
  private owned swExpr: Stopwatch;
  private owned swBlock: Stopwatch;
  public constructor()
    swExpr := Stopwatch.StartNew();
    swBlock := Stopwatch.StartNew();
  end constructor;

  public property SwExpr: Stopwatch get => swExpr; end;
  public property SwBlock: Stopwatch get => swBlock; end;

  public method Test(): int
    SwExpr.Start();
    SwExpr.Stop();
    SwExpr.Reset();
    SwBlock.Start();
    SwBlock.Stop();
    SwBlock.Reset();
    return 0;
  end Test;

  public static method Main(): int
    var c := new C();
    return c.Test();
  end Main;
end C;

// CHECK-NOT: error
// CHECK-NOT: warning
// CHECK-NOT: internal:
