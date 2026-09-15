//! case: s6_6_0009
//! doc: spec
//! section: 6.6
//! title: a double-typed result against a float target is a compile error
//! rule: In every other case the compound form is a compile error naming the
//!   explicit remedy — notably a binary-float or decimal result against a
//!   different LHS type (f += 2.5 where f: float promotes to double): write
//!   f := float(f + 2.5); when that loss is intended.
//! cites: 6.6, 6.2, 5.1
//! expect: reject
//! diagnostic: explicit remedy
module Conf;

public class Program
  public static method Main()
    var f: float := 1.0f;
    f += 2.5;
    Console.WriteLine(f);
  end Main;
end Program;
