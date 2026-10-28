//! case: l9_3_0015
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rejects surrounding whitespace
//! rule: stdlib §9.3 (v0.345): "**surrounding whitespace is not accepted** and raises
//!   `FormatException`".
//! cites: 9.3
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse(" 5"));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
