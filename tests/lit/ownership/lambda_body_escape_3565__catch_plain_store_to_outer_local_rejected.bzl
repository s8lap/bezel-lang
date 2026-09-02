// §31.1 (v0.286), issue #3565: the PLAIN-STORE spelling of the same assignment
// channel inside a lambda body — `holder := e;`, no closure anywhere.
//
// §31.1: "Exception instances themselves cannot escape the arm that caught
// them." fix #3520 gave this spelling its own wording at method level (the
// closure sentence would name a closure the program does not contain); it was
// equally unreachable inside a lambda body, for the same traversal reason.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class C
  public static method Run(): int
    var mk := lambda(): int => do
      var holder: Exception? := null;
      try
        throw new InvalidOperationException("wxyz");
      catch
        var e: Exception => do
          holder := e;
        end
      end try;
      return 0;
    end;
    return mk();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run().ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: caught exception `e` may not be assigned to `holder`
// CHECK-SAME: so a later read would see a disposed exception
// CHECK-SAME: (§31.1
