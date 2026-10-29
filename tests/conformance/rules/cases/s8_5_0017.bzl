//! case: s8_5_0017
//! doc: spec
//! section: 8.5
//! title: a stdlib enum renders its qualified type name through Object, a nullable hole, and null as "null"
//! rule: and for enums: the type's qualified name
//! cites: 8.5, 2.13
//! expect: run
//! output: System.DayOfWeek
//!   System.DayOfWeek
//!   System.DateTimeKind
//!   null
module Conf;

public class Program
  public static method Main()
    var o: Object := DayOfWeek.Friday;
    Console.WriteLine(o.ToString());
    Console.WriteLine($"{o}");
    var k: DateTimeKind? := DateTimeKind.Utc;
    Console.WriteLine($"{k}");
    var n: DayOfWeek? := null;
    Console.WriteLine($"{n}");
  end Main;
end Program;
