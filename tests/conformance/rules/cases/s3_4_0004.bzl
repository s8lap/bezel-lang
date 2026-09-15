//! case: s3_4_0004
//! doc: spec
//! section: 3.4
//! title: the keyword and PascalCase forms of a primitive name the same static members
//! rule: Static members of the standard library type are accessible through
//!   either form: int.Parse, int.MaxValue, double.IsNaN.
//! cites: 3.4
//! expect: run
//! stdout: 42
module Conf;

public class Program
  public static method Main()
    var x := Int32.Parse("42");
    Console.WriteLine($"{x}");
  end Main;
end Program;
