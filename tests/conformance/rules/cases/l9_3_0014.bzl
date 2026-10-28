//! case: l9_3_0014
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rejects an exponent
//! rule: stdlib §9.3 (v0.345): "§9.2's input grammar **without its exponent** … `"1e3"` [raises]
//!   `FormatException`".
//! cites: 9.3
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse("1e3"));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
