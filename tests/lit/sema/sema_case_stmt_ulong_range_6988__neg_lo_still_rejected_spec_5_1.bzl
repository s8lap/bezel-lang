// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6988 (§5.1, v0.340): the statement-form gate keeps rejecting a negative lower
// bound over a ulong scrutinee after the gate moved behind the arm walk.

module M;
public class Program
  public static method Main()
    var u: ulong := 3;
    case u of
      -5..10 => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// CHECK: :{{[0-9]+}}:{{[0-9]+}}: the `case` pattern constant `-5` has a type that is not implicitly convertible to the scrutinee type
// CHECK-SAME: (§5.1)
