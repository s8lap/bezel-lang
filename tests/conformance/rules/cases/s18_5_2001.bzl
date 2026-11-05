//! case: s18_5_2001
//! doc: spec
//! section: 18.5
//! title: a format spec on an enum hole is rejected: an enum is not
//!   formattable
//! rule: A format spec on an operand whose type is neither intrinsically
//!   formattable nor `IFormattable` is a compile error naming this
//!   rule.
//! cites: 10.5
//! expect: reject
//! diagnostic: formattable
module Conf;

enum E
  A, B;
end E;

public class Program
  public static method Main()
    var e := E.A;
    Console.WriteLine($"{e:F4}");
  end Main;
end Program;
