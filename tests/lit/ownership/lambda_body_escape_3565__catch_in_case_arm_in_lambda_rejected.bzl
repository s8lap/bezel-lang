// §31.1 "Throwing" / *Exception ownership* (v0.286), issue #3565 — the
// CASE-ARM axis, which is where two separate walker repairs meet.
//
// `narrow #3520` could not reject the method-level spelling of this program
// because `collectLocalDepthsStmt` had no `ast::CaseStmt` arm (issue #3552:
// a `case`-arm catch binding got no depth, so every consumer's
// `td >= 0 && sd >= 0` guard bailed). fix #3656 then put that walker on
// `forEachNestedStmt`, the single statement grammar, which does walk case
// arms. This row pins that the two repairs COMPOSE: a `case` arm, inside a
// `try`/`catch`, inside a LAMBDA BODY — the depth seed fix #3565 rebuilds for
// the lambda body reaches the arm binding through the shared grammar, so the
// §31.1 assignment channel fires here too.
//
// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s

module M;
import System;
class C
  public static method Run(k: int): int
    var mk := lambda(): int => do
      var f := lambda(): int => 0;
      case k of
        1 => do
          try
            throw new InvalidOperationException("wxyz");
          catch
            var e: Exception => do
              f := lambda(): int => e.Message.Length;
            end
          end try;
        end,
        _ => do end
      end case;
      return f() + f();
    end;
    return mk();
  end Run;
  public static method Main(): int
    Console.WriteLine(Run(1).ToString());
    return 0;
  end Main;
end C;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: caught exception `e` may not be captured by a closure assigned to `f`
// CHECK-SAME: (§31.1
