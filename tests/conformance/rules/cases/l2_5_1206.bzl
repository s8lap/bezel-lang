//! case: l2_5_1206
//! doc: stdlib
//! section: 2.5
//! title:Arrays of DateOnly and TimeOnly sort chronologically
//! rule:chronological; ordering is CompareTo only
//! cites: 4.4
//! expect: run
//! output: 0001-01-01
//!   1970-01-01
//!   2020-02-29
//!   2020-03-01
//!   9999-12-31
//!   00:00:00 0
//!   00:00:00 1
//!   12:00:00 0
//!   12:00:00 1
//!   23:59:59 999
module Conf;

public class Program
  public static method Main()
    var a: Array<DateOnly> := [new DateOnly(9999, 12, 31), new DateOnly(2020, 3, 1), new DateOnly(1, 1, 1), new DateOnly(2020, 2, 29), new DateOnly(1970, 1, 1)];
    a.Sort();
    for var x in a do
      Console.WriteLine(x);
    end for;
    var b: Array<TimeOnly> := [new TimeOnly(23, 59, 59, 999), new TimeOnly(0, 0), new TimeOnly(12, 0, 0, 1), new TimeOnly(12, 0), new TimeOnly(0, 0, 0, 1)];
    var s := b.Sorted();
    for var x in s do
      Console.WriteLine($"{x} {x.Millisecond}");
    end for;
  end Main;
end Program;
