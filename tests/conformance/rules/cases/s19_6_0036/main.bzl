//! case: s19_6_0036
//! doc: spec
//! section: 19.6
//! title: A cross-module exhaustive case over the System DayOfWeek and a DateTimeKind return round-trip
//! rule: Auto-imported System enums are the same type in every module (§2.3, v0.350, stdlib §2.5): a library whose method takes and returns DayOfWeek and decides it with a seven-arm `case` (exhaustive, §19.6) interoperates with another module that returns DayOfWeek and DateTimeKind. Next(Saturday) is Sunday 0; Next(Wednesday) is Thursday 4; DateTimeKind.Local is 2.
//! cites: 2.3, 2.5, 19.6, 2.8
//! expect: run
//! output: 0
//!   4
//!   2
//! issue: 7580
module App;

import Lib1;
import Lib2;

public class Program
  public static method Main()
    var d := L1.Next(DayOfWeek.Saturday);
    Console.WriteLine(int(byte(d)));
    var e: DayOfWeek := L2.D();
    Console.WriteLine(int(byte(L1.Next(e))));
    var k: DateTimeKind := L2.K();
    Console.WriteLine(int(byte(k)));
  end Main;
end Program;
