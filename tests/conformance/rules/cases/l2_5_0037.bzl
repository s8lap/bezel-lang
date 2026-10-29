//! case: l2_5_0037
//! doc: stdlib
//! section: 2.5
//! title: TimeSpan and DateTime members work on method parameters
//! rule: stdlib §2.5: CompareTo is tick order, ToString() is the constant "c" form (§2.18), Hours/TotalSeconds/Ticks as declared. For 3 hours against 1 hour: CompareTo is positive, "03:00:00", Hours 3, TotalSeconds 10800, adding to 2024-01-31 keeps day 31, and the difference is 108000000000 ticks.
//! cites: 2.5, 2.18
//! expect: run
//! stdout: True 03:00:00 3 10800 31 108000000000 108000000000
module Conf;

public class Program
  public static method F(a: TimeSpan, b: TimeSpan, d: DateTime)
    var d2 := d.Add(a);
    var t := d2.Subtract(d);
    Console.WriteLine($"{a.CompareTo(b) > 0} {a.ToString()} {a.Hours} {a.TotalSeconds} {d2.Day} {t.Ticks} {a.Ticks}");
  end F;

  public static method Main()
    F(TimeSpan.FromHours(3.0), TimeSpan.FromHours(1.0), new DateTime(2024, 1, 31));
  end Main;
end Program;
