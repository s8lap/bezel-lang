//! case: s22_6_0005
//! doc: spec
//! section: 22.6
//! title: example 2 -- two equally-ranked conversions are ambiguous
//! rule: "Two candidates that both require an implicit conversion at the
//!   same position are equally ranked there, regardless of which
//!   conversion each uses."
//! cites: 22.6
//! expect: reject
//! diagnostic: ambiguous
module Conf;

public class Converter
  public constructor()
  end constructor;

  public method Convert(n: long): string => "long";
  public method Convert(n: float): string => "float";
end Converter;

public class Program
  public static method Main()
    var cvt := new Converter();
    var a: int := 10;
    Console.WriteLine(cvt.Convert(a));
  end Main;
end Program;
