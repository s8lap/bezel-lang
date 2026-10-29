//! case: l9_1_0029
//! doc: stdlib
//! section: 9.1
//! title: a radix parse of a sign with no digits raises FormatException
//! rule: stdlib §9.1 (v0.350) "The radix alphabet": the input is "an optional leading
//!   `+` or `-`, then one or more digits of that radix, and nothing else". A bare
//!   `+` and a bare `-` have no digits, so each raises `FormatException`.
//! cites: 9.1
//! expect: run
//! output: FORMAT
//!   FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(int.Parse("+", 16));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
    try
      Console.WriteLine(int.Parse("-", 16));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
