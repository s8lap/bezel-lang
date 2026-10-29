//! case: l2_5_0043
//! doc: stdlib
//! section: 2.5
//! title: DateTime and TimeOnly named arguments are matched by name whatever their written order
//! rule: millisecond: int := 0, kind: DateTimeKind := DateTimeKind.Unspecified); ... public constructor(hour: int, minute: int, second: int := 0, millisecond: int := 0);
//! cites: 2.5, 22.5
//! expect: run
//! output: 6
//!   4
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2020, 1, 2, hour := 3, minute := 4, second := 5, millisecond := 6, kind := DateTimeKind.Local);
    Console.WriteLine(d.Millisecond);
    var t := new TimeOnly(hour := 1, minute := 2, millisecond := 3, second := 4);
    Console.WriteLine(t.Second);
  end Main;
end Program;
