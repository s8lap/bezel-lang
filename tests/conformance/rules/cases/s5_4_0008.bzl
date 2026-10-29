//! case: s5_4_0008
//! doc: spec
//! section: 5.4
//! title: An enum read from DateTime.DayOfWeek converts to int by explicit cast, also after a chained call
//! rule: Enums convert to their underlying integer types by explicit cast (spec 5.4); DayOfWeek.Friday is 5 and Saturday 6 (stdlib 2.5), and byte and long casts apply likewise.
//! cites: 5.4, 2.5
//! expect: run
//! output: 5
//!   6
//!   5
//!   5
//!   5
module Conf;

public class Program
  public static method Main()
    var dt := new DateTime(2024, 3, 15);
    Console.WriteLine(int(dt.DayOfWeek));
    Console.WriteLine(int(dt.AddDays(1).DayOfWeek));
    Console.WriteLine(byte(dt.DayOfWeek));
    Console.WriteLine(long(dt.DayOfWeek));
    var e := dt.DayOfWeek;
    var i: int := int(e);
    Console.WriteLine(i);
  end Main;
end Program;
