//! case: s22_6_0105
//! doc: spec
//! section: 22.6
//! title: a null literal reaching two nullable operator overloads separates neither and the call is ambiguous
//! rule: A null literal has no static type of its own (§14.5: it binds no type
//!   parameter, and a candidate it leaves unbound is inapplicable): it is
//!   applicable at every nullable parameter and at no non-nullable one, and is
//!   graded alike at every candidate it reaches, so it separates none.
//! cites: 22.6, 25.2
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public record Qt(X: int)
end Qt;

public record Pt(X: int, Y: int)
  public static pure method operator +(a: Pt?, b: Pt?): int => 1;
  public static pure method operator +(a: Pt?, b: Qt?): int => 2;
end Pt;

public class Program
  public static method Main()
    var p := new Pt(1, 1);
    Console.WriteLine(p + null);
  end Main;
end Program;
