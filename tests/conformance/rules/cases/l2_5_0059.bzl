//! case: l2_5_0059
//! doc: stdlib
//! section: 2.5
//! title: a Dictionary keyed by DateTime finds a key through its Local twin of equal Ticks
//! rule: A `DateTime`'s `=`, `<>`, `Equals` and `GetHashCode` read its `Ticks` alone, as
//!   `CompareTo` does: two values with equal `Ticks` are equal whatever their `Kind`s
//! cites: 2.5, 4.8
//! expect: run
//! stdout: True 7
module Conf;

public class Program
  public static method Main()
    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc);
    var l := DateTime.FromTicks(1000L, DateTimeKind.Local);
    using var d := new Dictionary<DateTime, int>();
    d.Add(u, 7);
    var has := d.ContainsKey(l);
    var v := d[l];
    Console.WriteLine($"{has} {v}");
  end Main;
end Program;
