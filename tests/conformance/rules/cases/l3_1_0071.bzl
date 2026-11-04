//! case: l3_1_0071
//! doc: stdlib
//! section: 3.1
//! title: StringBuilder.Capacity is never less than Length
//! rule: `Capacity`. The number of `char`s the builder holds storage for
//!   before its storage next grows. It is never less than `Length`, and never
//!   less than the `capacity` argument of the `StringBuilder(capacity)`
//!   constructor that made the builder (0 for the other two constructors).
//! cites: 3.1
//! expect: run
//! stdout: 1000 True
//! issue: 7697
module Conf;

public class Program
  public static method Main()
    var sb := new StringBuilder();
    for var i in 1..100 do
      sb.Append("abcdefghij");
    end;
    Console.WriteLine($"{sb.Length} {sb.Capacity >= sb.Length}");
  end Main;
end Program;
