//! case: l5_1_0009
//! doc: stdlib
//! section: 5.1
//! title: Min and Max over an Array<DateOnly> order chronologically
//! rule: chronological; ordering is CompareTo only
//! cites: 5.1, 2.5
//! expect: run
//! stdout: 2 12
module Conf;

public class Program
  public static method Main()
    var arr: Array<DateOnly> := [new DateOnly(2020, 12, 1), new DateOnly(2020, 2, 28), new DateOnly(2020, 7, 4)];
    Console.WriteLine($"{arr.Min().Month} {arr.Max().Month}");
  end Main;
end Program;
