//! case: s22_6_0103
//! doc: spec
//! section: 22.6
//! title: a null literal operand reaches only the Pt? operator overload; two Pt operands take the exact one
//! rule: A null literal has no static type of its own (§14.5: it binds no type
//!   parameter, and a candidate it leaves unbound is inapplicable): it is
//!   applicable at every nullable parameter and at no non-nullable one.
//! cites: 22.6, 25.2, 7.11
//! expect: run
//! output: 2
//!   2
//!   1
//! issue: 6939
module Conf;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt, b: Pt): int => 1;
  public static pure method operator +(a: Pt?, b: Pt?): int => 2;
end Pt;

public class Program
  public static method Main()
    var p := new Pt(1, 1);
    Console.WriteLine(p + null);
    Console.WriteLine(null + p);
    Console.WriteLine(p + p);
  end Main;
end Program;
