//! case: s9_3_0001
//! doc: spec
//! section: 9.3
//! title: a record may combine positional fields with a body that adds further members
//! rule: A record may have both positional fields and a body; the auto-generated
//!   members are present and the body adds further members.
//! expect: run
//! stdout: 4,6
module Conf;

public record Vector2(X: double, Y: double)
  public method Length(): double => Math.Sqrt(X * X + Y * Y);

  public static method operator +(a: Vector2, b: Vector2): Vector2 =>
    new Vector2(a.X + b.X, a.Y + b.Y);
end Vector2;

public class Program
  public static method Main()
    var a := new Vector2(1.0, 2.0);
    var b := new Vector2(3.0, 4.0);
    var c := a + b;
    Console.WriteLine($"{c.X},{c.Y}");
  end Main;
end Program;
