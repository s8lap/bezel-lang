//! case: l5_1_0008
//! doc: stdlib
//! section: 5.1
//! title: Min and Max over an Array<TimeSpan> order by ticks, negative spans first
//! rule: CompareTo = tick order. Ordering is CompareTo only
//! cites: 5.1, 2.5
//! expect: run
//! stdout: -50 300
//! issue: 8092
module Conf;

public class Program
  public static method Main()
    var arr: Array<TimeSpan> := [TimeSpan.FromTicks(300L), TimeSpan.FromTicks(-50L), TimeSpan.FromTicks(100L)];
    Console.WriteLine($"{arr.Min().Ticks} {arr.Max().Ticks}");
  end Main;
end Program;
