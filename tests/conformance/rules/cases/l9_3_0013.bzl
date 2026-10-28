//! case: l9_3_0013
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rejects a value with no integer digit
//! rule: stdlib §9.3 (v0.345): "`.5` … raise `FormatException`" — the grammar is one or more
//!   digits, then optionally a `.` and one or more digits.
//! cites: 9.3
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse(".5"));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
