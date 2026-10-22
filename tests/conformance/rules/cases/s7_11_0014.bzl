//! case: s7_11_0014
//! doc: spec
//! section: 7.11
//! title: a user-declared unary operator applies to a nullable record once a null test narrows it
//! rule: The rule reads the operand's static type, so an operand that §7.4 has
//!   narrowed non-null is a non-nullable operand and the ordinary operator
//!   applies to it unchanged; no separate rule is needed for that case.
//! cites: 7.11, 7.4, 25
//! expect: run
//! output: -1
//!   -2
module Conf;

public record Pt(X: int, Y: int)
  public static method operator -(a: Pt): Pt => new Pt(0 - a.X, 0 - a.Y);
end Pt;

public class Program
  public static method Make(): Pt? => new Pt(1, 2);

  public static method Main()
    var p: Pt? := Make();
    if p <> null then
      var r := -p;
      Console.WriteLine(r.X);
      Console.WriteLine(r.Y);
    end;
  end Main;
end Program;
