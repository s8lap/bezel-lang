//! case: l5_1_0007
//! doc: stdlib
//! section: 5.1
//! title: Min and Max over DateTime order by Ticks without normalizing the Kind
//! rule: CompareTo = tick order (Kind not
//! cites: 5.1, 2.5
//! expect: run
//! stdout: 100 300 True
module Conf;

public class Program
  public static method Main()
    var arr: Array<DateTime> := [DateTime.FromTicks(300L), DateTime.FromTicks(100L, DateTimeKind.Utc), DateTime.FromTicks(200L, DateTimeKind.Local)];
    var lo := arr.Min();
    var hi := arr.Max();
    var loUtc := lo.Kind = DateTimeKind.Utc;
    Console.WriteLine($"{lo.Ticks} {hi.Ticks} {loUtc}");
  end Main;
end Program;
