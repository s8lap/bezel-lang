//! case: l3_1_0072
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Capacity is never less than the capacity constructor argument
//! rule: `Capacity`. The number of `char`s the builder holds storage for
//!   before its storage next grows. It is never less than `Length`, and never
//!   less than the `capacity` argument of the `StringBuilder(capacity)`
//!   constructor that made the builder (0 for the other two constructors).
//! cites: 3.1
//! expect: run
//! stdout: True True
//! issue: 7697
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder(64);
    var before := sb.Capacity >= 64;
    sb.Append("abc");
    Console.WriteLine($"{before} {sb.Capacity >= 64}");
  end Main;
end Program;
