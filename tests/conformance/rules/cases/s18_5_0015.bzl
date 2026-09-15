//! case: s18_5_0015
//! doc: spec
//! section: 18.5
//! title: an unrecognized format specifier raises FormatException at runtime
//! rule: "An unrecognized specifier at runtime raises `FormatException` (the
//!   defined-fault rule, stdlib §2.18)."
//! cites: 18.5
//! expect: run
//! stdout: format
module Conf;

public class Program
  public static method Main()
    var value := 42;
    try
      Console.WriteLine($"{value:Q}");
    catch
      var ex: FormatException => Console.WriteLine("format")
    end try;
  end Main;
end Program;
