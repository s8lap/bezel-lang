//! case: s19_3_0019
//! doc: spec
//! section: 19.3
//! title: a naked name directly after = in a named slot is a constant reference, also one level down
//! rule: A naked identifier on a named slot's right-hand side is not a binding. Directly after = it resolves as at the top level of a pattern: a constant reference or a type test by name resolution. The rule applies inside a nested named slot too, so Outer(I = Inner(X = W)) with a const W is an equality test on X.
//! cites: 19.3
//! expect: run
//! output: a-eq
//!   b-n
//! issue: 6037
module Conf;

public record Inner(X: int);

public record Outer(I: Inner);

public class Program
  public static method Main()
    const W: int := 3;
    var a := new Outer(new Inner(3));
    var b := new Outer(new Inner(5));
    case a of Outer(I = Inner(X = W)) => Console.WriteLine("a-eq");,
      _ => Console.WriteLine("a-n");
    end case;
    case b of Outer(I = Inner(X = W)) => Console.WriteLine("b-eq");,
      _ => Console.WriteLine("b-n");
    end case;
  end Main;
end Program;
