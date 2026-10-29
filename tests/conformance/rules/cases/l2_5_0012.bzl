//! case: l2_5_0012
//! doc: stdlib
//! section: 2.5
//! title: DateTime, DateOnly and TimeOnly constructors admit every arity and naming they declare
//! rule: DateTime declares `constructor(year, month, day)` and `constructor(year, month, day, hour, minute,
//!   second, millisecond := 0, kind := ...)`; `DateOnly` declares `constructor(year, month, day)`; `TimeOnly`
//!   declares `constructor(hour, minute, second := 0, millisecond := 0)`. Named arguments may appear in any
//!   order after positional ones and every parameter without a default must be bound (spec 22.5).
//! cites: 2.5, 22.5, 22.6
//! expect: run
//! output: 3
//!   4
module Conf;

public class Program
  public static method Main()
    var a01 := new DateTime(2024, 3, 4);
    var a02 := new DateTime(2024, 3, 4, 5, 6, 7);
    var a03 := new DateTime(2024, 3, 4, 5, 6, 7, 8);
    var a04 := new DateTime(year := 1, month := 2, day := 3);
    var a05 := new DateTime(2024, 3, 4, 5, 6, second := 7);
    var a06 := new DateTime(2024, 3, 4, 5, 6, 7, millisecond := 8);
    var a07 := new DateOnly(2024, 3, 4);
    var a08 := new DateOnly(day := 1, year := 2, month := 3);
    var a09 := new TimeOnly(1, 2);
    var a10 := new TimeOnly(1, 2, 3);
    var a11 := new TimeOnly(1, 2, 3, 4);
    var a12 := new TimeOnly(1, minute := 2);
    var a13 := new TimeOnly(1, 2, millisecond := 4);
    Console.WriteLine(a04.Day);
    Console.WriteLine(a06.Day);
  end Main;
end Program;
