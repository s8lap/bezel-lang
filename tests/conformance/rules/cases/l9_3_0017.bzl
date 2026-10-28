//! case: l9_3_0017
//! doc: stdlib
//! section: 9.3
//! title: decimal.Parse rejects a second decimal point
//! rule: stdlib §9.3 (v0.345): "`"1.2.3"` [raises] `FormatException`" — the grammar admits at
//!   most one `.` followed by one or more digits.
//! cites: 9.3
//! expect: run
//! stdout: FORMAT
module Conf;

public class Program
  public static method Main()
    try
      Console.WriteLine(decimal.Parse("1.2.3"));
    catch
      FormatException => Console.WriteLine("FORMAT");
    end;
  end Main;
end Program;
