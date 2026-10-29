//! case: s5_1_0151
//! doc: spec
//! section: 5.1
//! title: A DayOfWeek property value is not implicitly convertible to int in a variable initializer
//! rule: DateTime.DayOfWeek has type DayOfWeek; an enum never converts implicitly to its underlying integer (spec 5.1, 5.4).
//! cites: 5.1, 5.4
//! expect: reject
//! diagnostic: not implicitly convertible
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 3, 15);
    var n: int := d.DayOfWeek;
    Console.WriteLine(n);
  end Main;
end Program;
