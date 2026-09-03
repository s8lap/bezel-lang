// §31.1 (v0.286), issue #3707 — ACCEPT control, the exact boundary the fix
// must not cross.
//
// Same nested-lambda-inside-catch-arm shape as the REJECT row in this pair
// (lambda_body_escape_3707__catch_closure_assign_to_method_scope_local_rejected.bzl),
// but `f` is declared INSIDE `mk`'s own body instead of at method scope.
// `collectLocalDepths(mk.stmts)` DOES visit `f`'s decl this time, so its
// depth is found (`td >= 0`) and the fix's new branch — gated on `td < 0` —
// never fires; the assignment falls through to the pre-existing
// `else { return false; }`, exactly as it did before #3707.
//
// This is genuinely safe, not merely unflagged: `f` dies with `mk`'s own
// invocation, which runs to completion (both calls to the reassigned `f`
// included) strictly inside the dynamic extent of the arm that calls `mk()`.
//
// Clean-compile ACCEPT control: bezelc -c must exit zero and emit no
// diagnostics.
//
// RUN: %bezelc -c -o %t.o %s
// RUN: %bezelc -c %s 2>&1 | FileCheck --check-prefix=NODIAG --allow-empty %s

module M;
import System;
class C
  public static method Run(): int
    var total := 0;
    try
      throw new InvalidOperationException("wxyz");
    catch
      var e: Exception => do
        var mk := lambda(): int => do
          var f := lambda(): int => 0;
          f := lambda(): int => e.Message.Length;
          return f() + f();
        end;
        total := mk();
      end
    end try;
    return total;
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// NODIAG-NOT: error
// NODIAG-NOT: §
