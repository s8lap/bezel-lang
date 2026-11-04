//! case: l5_1_0006
//! doc: stdlib
//! section: 5.1
//! title: Min and Max over an Array<DateTime> pick the earliest and latest value
//! rule: `Min` and `Max` without a selector are generic over comparable types:
//! cites: 5.1, 2.5
//! expect: run
//! output: 2019
//!   2021
//! issue: 8092
module Conf;

public class Program
  public static method Main()
    var arr: Array<DateTime> := [new DateTime(2021, 1, 1), new DateTime(2019, 1, 1), new DateTime(2020, 1, 1)];
    Console.WriteLine(arr.Min().Year);
    Console.WriteLine(arr.Max().Year);
  end Main;
end Program;
