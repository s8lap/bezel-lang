//! case: l2_5_0058
//! doc: stdlib
//! section: 2.5
//! title: DateTime.GetHashCode agrees across Kinds at equal Ticks
//! rule: A `DateTime`'s `=`, `<>`, `Equals` and `GetHashCode` read its `Ticks` alone, as
//!   `CompareTo` does: two values with equal `Ticks` are equal whatever their `Kind`s
//! cites: 2.5, 8.3
//! expect: run
//! stdout: True
module Conf;

public class Program
  public static method Main()
    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc);
    var l := DateTime.FromTicks(1000L, DateTimeKind.Local);
    var hu := u.GetHashCode();
    var hl := l.GetHashCode();
    Console.WriteLine(hu = hl);
  end Main;
end Program;
