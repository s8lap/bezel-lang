//! case: s22_6_0007
//! doc: spec
//! section: 22.6
//! title: example 4 -- each candidate exact on a different argument is ambiguous
//! rule: "If no candidate is unambiguously better — because each of two
//!   candidates is exact on a different argument ... the call is
//!   ambiguous and is a compile error."
//! cites: 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Calculator
  public constructor()
  end constructor;

  public method Compute(a: int, b: long): string => "int,long";
  public method Compute(a: long, b: int): string => "long,int";
end Calculator;

public class Program
  public static method Main()
    var calc := new Calculator();
    var x: int := 5;
    var y: int := 10;
    Console.WriteLine(calc.Compute(x, y));
  end Main;
end Program;
