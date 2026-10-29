//! case: l2_5_0045
//! doc: stdlib
//! section: 2.5
//! title: DateTime constructors raise on invalid named components and accept valid named ones in any order
//! rule: ArgumentOutOfRangeException on invalid components
//! cites: 2.5, 22.5
//! expect: run
//! output: caught named
//!   caught named2
//!   5
module Conf;

public class Program
  public static method Main()
    try
      var d := new DateTime(day := 40, month := 1, year := 2020);
      Console.WriteLine(d.Day);
    catch ArgumentOutOfRangeException => Console.WriteLine("caught named"); end;
    try
      var d := new DateTime(second := 1, minute := 2, hour := 25, day := 1, month := 1, year := 2020);
      Console.WriteLine(d.Day);
    catch ArgumentOutOfRangeException => Console.WriteLine("caught named2"); end;
    var d3 := new DateTime(hour := 5, day := 1, month := 1, year := 2020, minute := 1, second := 1);
    Console.WriteLine(d3.Hour);
  end Main;
end Program;
