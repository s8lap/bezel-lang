//! case: s18_5_0009
//! doc: spec
//! section: 18.5
//! title: a format spec on a non-formattable, non-IFormattable operand is a compile error
//! rule: "A format spec on an operand whose type is neither intrinsically
//!   formattable nor `IFormattable` is a compile error naming this rule."
//! cites: 18.5
//! expect: reject
//! diagnostic: formattable
module Conf;

public class Plain
  public constructor()
  end constructor;
end Plain;

public class Program
  public static method Main()
    var p := new Plain();
    Console.WriteLine($"{p:X}");
  end Main;
end Program;
