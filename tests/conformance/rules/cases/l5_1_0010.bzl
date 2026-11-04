//! case: l5_1_0010
//! doc: stdlib
//! section: 5.1
//! title: Min and Max over an Array<TimeOnly> order by time of day
//! rule: public method CompareTo(other: TimeOnly): int;
//! cites: 5.1, 2.5
//! expect: run
//! stdout: 1 3
module Conf;

public class Program
  public static method Main()
    var arr: Array<TimeOnly> := [new TimeOnly(3, 0), new TimeOnly(1, 0), new TimeOnly(2, 0)];
    Console.WriteLine($"{arr.Min().Hour} {arr.Max().Hour}");
  end Main;
end Program;
