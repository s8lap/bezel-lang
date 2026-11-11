//! case: l9_1_4610
//! doc: stdlib
//! section: 9.1
//! title: long.Parse returns Int64.MinValue for its own spelling
//! rule: a value outside the type's range raising `ParseRangeException`
//! cites: 9.1
//! expect: run
//! stdout: -9223372036854775808
module Conf;

public class Program
  public static method Main()
    Console.WriteLine(long.Parse("-9223372036854775808"));
  end Main;
end Program;
