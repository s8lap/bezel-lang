//! case: s19_3_0023
//! doc: spec
//! section: 19.3
//! title: a naked name in a named slot of a choice payload nested under a named slot is a constant reference
//! rule: A naked identifier on a named slot's right-hand side is not a binding. Directly after = it resolves as a constant reference by name resolution, at every depth. Outer(I = Sq(Side = W)) with a const W is an equality test on the choice payload's Side.
//! cites: 19.3
//! expect: run
//! output: a-eq
//!   b-n
//! issue: 6037
module Conf;

public choice Shape Sq(Side: int), Tri(Base: int, H: int); end Shape;

public record Outer(I: Shape, N: int);

public class Program
  public static method Main()
    const W: int := 3;
    var a := new Outer(Shape.Sq(3), 1);
    var b := new Outer(Shape.Sq(9), 1);
    case a of Outer(I = Sq(Side = W)) => Console.WriteLine("a-eq");,
      _ => Console.WriteLine("a-n");
    end case;
    case b of Outer(I = Sq(Side = W)) => Console.WriteLine("b-eq");,
      _ => Console.WriteLine("b-n");
    end case;
  end Main;
end Program;
