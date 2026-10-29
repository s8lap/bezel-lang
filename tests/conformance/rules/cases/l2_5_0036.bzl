//! case: l2_5_0036
//! doc: stdlib
//! section: 2.5
//! title: DateTime Add/Subtract/AddDays and the + and - operators with a TimeSpan agree
//! rule: stdlib §2.5 declares DateTime.Add(span), Subtract(span), Subtract(other), AddDays(days) and the operators `d + t`, `t + d`, `d - t`, `a - b`. From 2024-01-31: +1 day is day 1 (Feb 1); -1 day is day 30; the difference is 864000000000 ticks (10,000,000 ticks per second times 86,400, §2.5); AddDays(2.0) is day 2.
//! cites: 2.5
//! expect: run
//! stdout: 1 30 864000000000 2 1 1 30 864000000000
module Conf;

public class Program
  public static method Main()
    var d := new DateTime(2024, 1, 31);
    var s := TimeSpan.FromDays(1.0);
    var d1 := d.Add(s);
    var d2 := d.Subtract(s);
    var t := d1.Subtract(d);
    var d3 := d.AddDays(2.0);
    var d4 := d + s;
    var d5 := s + d;
    var d6 := d - s;
    var t2 := d4 - d;
    Console.WriteLine($"{d1.Day} {d2.Day} {t.Ticks} {d3.Day} {d4.Day} {d5.Day} {d6.Day} {t2.Ticks}");
  end Main;
end Program;
