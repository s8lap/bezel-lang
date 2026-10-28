//! case: l9_3_0016
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rejects a digit separator
//! rule: stdlib §9.3 (v0.345): "`"1_000"` [raises] `FormatException`" — the grammar admits the
//!   digits `0`–`9` and nothing else.
//! cites: 9.3
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse("1_000"));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
