//! case: l2_5_2103
//! doc: stdlib
//! section: 2.5
//! title: a DateTime viewed as IComparable and IEquatable ignores Kind and compares by Ticks
//! rule: two values with equal `Ticks` are equal whatever their `Kind`s, so `a = b` exactly where `a.CompareTo(b) = 0` (matching .NET).
//! cites: 2.8, 2.9
//! expect: run
//! output: True
//!   True
//!   False
//! issue: 7875
module Conf;
public class Program
  public static method Main()
    var utc := new DateTime(2020, 1, 2, 3, 4, 5, kind := DateTimeKind.Utc);
    var unspec := new DateTime(2020, 1, 2, 3, 4, 5);
    var later := new DateTime(2020, 1, 2, 3, 4, 6);
    var ic: IComparable<DateTime> := utc;
    var eq: IEquatable<DateTime> := utc;
    Console.WriteLine(ic.CompareTo(unspec) = 0);
    Console.WriteLine(eq.Equals(unspec));
    Console.WriteLine(eq.Equals(later));
  end Main;
end Program;
