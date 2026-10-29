//! case: l9_1_0026
//! doc: stdlib
//! section: 9.1
//! title: the radix parse rejects the empty string
//! rule: stdlib §9.1 (v0.350) "The radix alphabet": the input is "an optional leading `+` or `-`,
//!   then one or more digits of that radix, and nothing else", and "any other input raises
//!   `FormatException`". The empty string has no digits, so it is other input.
//! cites: 9.1
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(int.Parse("", 16));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
