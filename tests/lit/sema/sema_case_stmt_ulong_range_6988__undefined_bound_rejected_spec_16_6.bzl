// RUN: %not %bezelc -c %s 2>&1 | FileCheck %s
//
// #6988 (§19.3, v0.340): a statement-form range bound that names no declaration
// is rejected. Before the fix the statement form never typed range bounds, so
// `0..Undefined` over a ulong compiled and printed nothing; the expression form
// already rejected it. The arm walk now types the bounds in both forms.

module M;
public class Program
  public static method Main()
    var u: ulong := 3;
    case u of
      0..Undefined => Console.WriteLine("in"),
      _ => Console.WriteLine("out")
    end case;
  end Main;
end Program;

// CHECK: unresolved identifier `Undefined`
