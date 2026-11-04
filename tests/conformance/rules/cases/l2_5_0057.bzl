//! case: l2_5_0057
//! doc: stdlib
//! section: 2.5
//! title: DateTime.Equals is true across Kinds at equal Ticks and false at unequal Ticks
//! rule: A `DateTime`'s `=`, `<>`, `Equals` and `GetHashCode` read its `Ticks` alone, as
//!   `CompareTo` does: two values with equal `Ticks` are equal whatever their `Kind`s
//! cites: 2.5, 8.3
//! expect: run
//! stdout: True False
module Conf;

public class Program
  public static method Main()
    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc);
    var l := DateTime.FromTicks(1000L, DateTimeKind.Local);
    var other := DateTime.FromTicks(1001L, DateTimeKind.Utc);
    var e1 := u.Equals(l);
    var e2 := u.Equals(other);
    Console.WriteLine($"{e1} {e2}");
  end Main;
end Program;
