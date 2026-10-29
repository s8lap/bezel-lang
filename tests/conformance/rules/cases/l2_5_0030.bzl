//! case: l2_5_0030
//! doc: stdlib
//! section: 2.5
//! title: DateTime, DateOnly, TimeOnly constructors bind named and defaulted arguments by parameter
//! rule: DateTime declares `constructor(year, month, day, hour, minute, second, millisecond := 0, kind := ...)`;
//!   `TimeOnly` declares `constructor(hour, minute, second := 0, millisecond := 0)`; a named argument binds
//!   the parameter of that name in any order (spec 22.5), and an omitted defaulted parameter takes its
//!   default (0).
//! cites: 2.5, 22.5
//! expect: run
//! output: 1 2 0 5
//!   1 2 0 5
//!   1 2 0 0
//!   1 2 9 0
//!   1 2 9 5
//!   1 2 0 0
//!   2024-3-4 5:6:7.9
//!   2024-3-4 5:6:7.0
//!   2024-3-4 5:6:7.0
//!   2024-3-4 0:0:0
//!   2024-3-4 2024-3-4
module Conf;

public class Program
  public static method Main()
    var t1 := new TimeOnly(hour := 1, minute := 2, millisecond := 5);
    var t2 := new TimeOnly(1, 2, millisecond := 5);
    var t3 := new TimeOnly(minute := 2, hour := 1);
    var t4 := new TimeOnly(1, 2, second := 9);
    var t5 := new TimeOnly(1, 2, 9, 5);
    var t6 := new TimeOnly(1, 2);
    var d1 := new DateTime(2024, 3, 4, 5, 6, 7, millisecond := 9);
    var d2 := new DateTime(2024, 3, 4, 5, 6, 7);
    var d3 := new DateTime(day := 4, year := 2024, month := 3, second := 7, minute := 6, hour := 5);
    var d4 := new DateTime(year := 2024, month := 3, day := 4);
    var o1 := new DateOnly(2024, 3, 4);
    var o2 := new DateOnly(day := 4, month := 3, year := 2024);
    Console.WriteLine($"{t1.Hour} {t1.Minute} {t1.Second} {t1.Millisecond}");
    Console.WriteLine($"{t2.Hour} {t2.Minute} {t2.Second} {t2.Millisecond}");
    Console.WriteLine($"{t3.Hour} {t3.Minute} {t3.Second} {t3.Millisecond}");
    Console.WriteLine($"{t4.Hour} {t4.Minute} {t4.Second} {t4.Millisecond}");
    Console.WriteLine($"{t5.Hour} {t5.Minute} {t5.Second} {t5.Millisecond}");
    Console.WriteLine($"{t6.Hour} {t6.Minute} {t6.Second} {t6.Millisecond}");
    Console.WriteLine($"{d1.Year}-{d1.Month}-{d1.Day} {d1.Hour}:{d1.Minute}:{d1.Second}.{d1.Millisecond}");
    Console.WriteLine($"{d2.Year}-{d2.Month}-{d2.Day} {d2.Hour}:{d2.Minute}:{d2.Second}.{d2.Millisecond}");
    Console.WriteLine($"{d3.Year}-{d3.Month}-{d3.Day} {d3.Hour}:{d3.Minute}:{d3.Second}.{d3.Millisecond}");
    Console.WriteLine($"{d4.Year}-{d4.Month}-{d4.Day} {d4.Hour}:{d4.Minute}:{d4.Second}");
    Console.WriteLine($"{o1.Year}-{o1.Month}-{o1.Day} {o2.Year}-{o2.Month}-{o2.Day}");
  end Main;
end Program;
