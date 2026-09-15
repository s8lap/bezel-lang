//! case: s25_3_0003
//! doc: spec
//! section: 25.3
//! title: an overloaded operator is invoked by the operator syntax
//! rule: For `a <op> b` on non-primitive operands the candidate set is drawn from
//!   the operator overloads declared on the static types of both operands.
//! cites: 25.2, 25.3
//! expect: run
//! stdout: 4
module Conf;

public record Vec2(X: int, Y: int)
  public static method operator +(a: Vec2, b: Vec2): Vec2 =>
    new Vec2(a.X + b.X, a.Y + b.Y);
end Vec2;

public class Program
  public static method Main()
    var a := new Vec2(1, 2);
    var b := new Vec2(3, 4);
    var c := a + b;
    Console.WriteLine(c.X);
  end Main;
end Program;
