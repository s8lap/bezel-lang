//! case: l2_5_0049
//! doc: stdlib
//! section: 2.5
//! title: the shorthand new(...) with an annotated DateTime or DateOnly local raises on an invalid component
//! rule: ArgumentOutOfRangeException on invalid components
//! cites: 2.5, 17.7
//! expect: run
//! output: caught shorthand
//!   caught shorthand2
module Conf;

public class Program
  public static method Main()
    try
      var d: DateTime := new(2020, 13, 1);
      Console.WriteLine(d.Day);
    catch ArgumentException => Console.WriteLine("caught shorthand"); end;
    try
      var d2: DateOnly := new(2020, 1, 33);
      Console.WriteLine(d2.Day);
    catch ArgumentException => Console.WriteLine("caught shorthand2"); end;
  end Main;
end Program;
