//! case: s5_1_0154
//! doc: spec
//! section: 5.1
//! title: A DayOfWeek value is not implicitly convertible to int as a return value
//! rule: DateTime.DayOfWeek has type DayOfWeek; an enum never converts implicitly to its underlying integer, so returning it from an int method is rejected (spec 5.1, 5.4).
//! cites: 5.1, 5.4
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method F(): int
    var dt := new DateTime(2024, 3, 15);
    return dt.DayOfWeek;
  end F;
  public static method Main()
    Console.WriteLine(Program.F());
  end Main;
end Program;
