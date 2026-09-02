// §31.1 "Exception ownership" (v0.286), issue #3565: the ASSIGNMENT channel of
// the caught-exception escape rule, reached inside a LAMBDA BODY.
//
// §31.1: the bound exception variable "is a **borrow** of the in-flight
// exception; its source is the runtime, and its lifetime is the arm body's",
// it may not be "captured by a closure that may outlive the arm", and
// "Exception instances themselves cannot escape the arm that caught them."
//
// The rejection has existed at METHOD level since fix #3520 (it needed the
// catch arm binding's own block depth in `localDepth_`). Inside a lambda body
// it was unreachable on TWO counts: ownership's statement walks never enter a
// lambda body (a lambda body lives inside a `LambdaExpr`, an expression), and
// `localDepth_` held the method's blocks only. Measured before fix #3565:
// this program compiled rc=0 with no diagnostic and died with a
// heap-use-after-free (rc=139; ASan: "heap-use-after-free ... in lam$2") when
// `f` was called after the arm completed.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class C
  public static method Run(): int
    var mk := lambda(): int => do
      var f := lambda(): int => 0;
      try
        throw new InvalidOperationException("wxyz");
      catch
        var e: Exception => do
          f := lambda(): int => e.Message.Length;
        end
      end try;
      return f() + f();
    end;
    return mk();
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
