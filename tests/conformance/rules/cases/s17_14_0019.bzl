//! case: s17_14_0019
//! doc: spec
//! section: 17.14
//! title: a constant expression whose mixed int/uint subtraction falls below zero is a compile
//!   error
//! rule: "An evaluation fault - integer or decimal overflow, division or mod by zero ... in a
//!   constant expression is a compile error" (§17.14, v0.340). `1 - 2U` is computed in uint
//!   (§6.2), where -1 is outside the range.
//! cites: 6.2
//! expect: reject
//! diagnostic: constant
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(1 - 2U);
  end Main;
end Program;
