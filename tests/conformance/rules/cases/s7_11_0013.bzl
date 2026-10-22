//! case: s7_11_0013
//! doc: spec
//! section: 7.11
//! title: a user-declared unary operator is not applicable to an un-narrowed nullable record
//! rule: A user-declared `operator` on a record or class type (§25) is likewise not
//!   defined on a nullable operand: such an operand at a user-defined-operator
//!   position is the same compile error, naming the same remedies.
//! cites: 7.11, 25
//! expect: reject
//! diagnostic: nullable
module Conf;

public record Pt(X: int, Y: int)
  public static method operator -(a: Pt): Pt => new Pt(0 - a.X, 0 - a.Y);
end Pt;

public class Program
  public static method Make(): Pt? => new Pt(1, 2);

  public static method Main()
    var p: Pt? := Make();
    var r := -p;
    Console.WriteLine(r.X);
  end Main;
end Program;
