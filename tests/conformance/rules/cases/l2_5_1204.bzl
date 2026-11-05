//! case: l2_5_1204
//! doc: stdlib
//! section: 2.5
//! title:DateTime ordering reads Ticks alone: Sorted and Sort both ignore Kind
//! rule:CompareTo = tick order (Kind not
//! cites: 4.4
//! expect: run
//! output: 5 True
//!   10 False
//!   20 False
//!   30 True
//!   5 True
//!   10 False
//!   20 False
//!   30 True
module Conf;

public class Program
  public static method Main()
    var a: Array<DateTime> := [DateTime.FromTicks(30L, DateTimeKind.Local), DateTime.FromTicks(20L, DateTimeKind.Utc), DateTime.FromTicks(10L, DateTimeKind.Unspecified), DateTime.FromTicks(5L, DateTimeKind.Local)];
    var s := a.Sorted();
    for var x in s do
      Console.WriteLine($"{x.Ticks} {x.Kind = DateTimeKind.Local}");
    end for;
    a.Sort();
    for var x in a do
      Console.WriteLine($"{x.Ticks} {x.Kind = DateTimeKind.Local}");
    end for;
  end Main;
end Program;
