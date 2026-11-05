//! case: l2_5_1207
//! doc: stdlib
//! section: 2.5
//! title:DateTimes with equal Ticks and different Kinds sort as ties without losing an element
//! rule:CompareTo = tick order (Kind not
//! cites: 4.4
//! expect: run
//! output: 1
//!   5
//!   5
//!   5
module Conf;

public class Program
  public static method Main()
    var a: Array<DateTime> := [DateTime.FromTicks(5L), DateTime.FromTicks(5L, DateTimeKind.Utc), DateTime.FromTicks(1L), DateTime.FromTicks(5L, DateTimeKind.Local)];
    a.Sort();
    for var x in a do
      Console.WriteLine(x.Ticks);
    end for;
  end Main;
end Program;
