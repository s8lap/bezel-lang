//! case: s8_5_0019
//! doc: spec
//! section: 8.5
//! title: a generic hole and a concatenated ToString render stdlib and user enums by qualified name
//! rule: and for enums: the type's qualified name
//! cites: 8.5, 11.8
//! expect: run
//! output: System.DayOfWeek
//!   Conf.Color
//!   x=System.DayOfWeek
//!   True
module Conf;

enum Color
  Red, Green;
end Color;

class Util
  public static method Show<T>(x: T): string => $"{x}";
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.Show(DayOfWeek.Saturday));
    Console.WriteLine(Util.Show(Color.Red));
    Console.WriteLine("x=" + DayOfWeek.Sunday.ToString());
    Console.WriteLine(DateTimeKind.Utc.ToString() = "System.DateTimeKind");
  end Main;
end Program;
