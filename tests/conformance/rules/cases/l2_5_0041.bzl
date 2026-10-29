//! case: l2_5_0041
//! doc: stdlib
//! section: 2.5
//! title: DateOnly binds positional then named arguments, and all-named arguments in reverse order
//! rule: public constructor(year: int, month: int, day: int);   // ArgumentOutOfRangeException on invalid
//! cites: 2.5, 22.5
//! expect: run
//! output: 2
//!   2021-4-3
module Conf;

public class Program
  public static method Main()
    var x := new DateOnly(2020, 1, day := 2);
    Console.WriteLine(x.Day);
    var y := new DateOnly(day := 3, month := 4, year := 2021);
    Console.WriteLine($"{y.Year}-{y.Month}-{y.Day}");
  end Main;
end Program;
