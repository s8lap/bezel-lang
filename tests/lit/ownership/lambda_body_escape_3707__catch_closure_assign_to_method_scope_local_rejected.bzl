// §31.1 "Exception ownership" (v0.286), issue #3707 — residual of #3565.
//
// #3565 reached the ASSIGNMENT channel of the caught-exception escape rule
// inside a lambda body, but only for a target declared INSIDE that lambda
// body: `checkOneLambdaBodyEscapes` re-seeds `localDepth_` from the lambda
// body ALONE, so a target at METHOD scope (or in an outer lambda's own body)
// is never visited by that re-seeding walk and its depth comes back -1. The
// pre-existing `td >= 0 && sd >= 0` depth-comparison guard then silently
// declined — the same dead-arm shape issue #3520 already named, now measured
// for this scope.
//
// This file is the a3-shaped repro kept for review at
// /var/tmp/bezel-scratch/r12/review-c5-b3/a3_hoisted_f.bzl: `f` is declared
// at method scope, the `try`/`catch` is ALSO at method scope, and the lambda
// under check (`mk`) is declared *inside* the catch arm — so the arm is an
// ANCESTOR of `mk.stmts`, not a descendant, and even the source's own depth
// is unresolved inside `mk`'s re-seeded scope. `isCatchBorrow` needs no depth
// at all (a scope-free sema flag), so the fix does not depend on it either.
//
// Measured on the parent (pre-#3707) build: compiles rc=0 with NO diagnostic,
// runs rc=139 (SIGSEGV) — `f`'s closure reads the caught exception `e` after
// the runtime has disposed it at the end of the arm.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class C
  public static method Run(): int
    var f := lambda(): int => 0;
    var total := 0;
    try
      throw new InvalidOperationException("wxyz");
    catch
      var e: Exception => do
        var mk := lambda(): int => do
          f := lambda(): int => e.Message.Length;
          return 0;
        end;
        total := mk();
      end
    end try;
    return total + f() + f();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// The diagnostic is the METHOD-level one, unchanged — the same check, reached
// through a scope it could not reach before, not a second spelling of the rule.
// CHECK: :{{[0-9]+}}:{{[0-9]+}}: caught exception `e` may not be captured by a closure assigned to `f`
// CHECK-SAME: lives in an enclosing block that outlives the arm
// CHECK-SAME: (§31.1
