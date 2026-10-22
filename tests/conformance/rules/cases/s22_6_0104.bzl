//! case: s22_6_0104
//! doc: spec
//! section: 22.6
//! title: a null literal operand is not applicable at an operator whose formals are non-nullable
//! rule: A null literal has no static type of its own (§14.5: it binds no type
//!   parameter, and a candidate it leaves unbound is inapplicable): it is
//!   applicable at every nullable parameter and at no non-nullable one.
//! cites: 22.6, 25.2, 7.11
//! expect: reject
//! diagnostic: is not defined for type `Object`
module Conf;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt, b: Pt): int => 1;
end Pt;

public class Program
  public static method Main()
    var p := new Pt(1, 1);
    Console.WriteLine(p + null);
  end Main;
end Program;
