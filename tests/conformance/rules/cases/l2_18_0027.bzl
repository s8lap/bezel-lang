//! case: l2_18_0027
//! doc: stdlib
//! section: 2.18
//! title: an unrecognized specifier on an integer raises FormatException
//! rule: The specifier vocabulary is invariant-culture only — Bezel has no culture
//!   machinery, and formatted output is identical on every platform. An
//!   unrecognized specifier raises `FormatException` (strict-fault, as everywhere).
//! cites: 2.18, 9.2
//! expect: run
//! issue: 7303
//! stdout: FMT

module Conf;

public class Program
  public static method Main()
    var i: int := 255;
    try
      Console.WriteLine(i.ToString("Q"));
    catch
      FormatException => Console.WriteLine("FMT");
    end;
  end Main;
end Program;
