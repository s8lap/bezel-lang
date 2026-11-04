//! case: l2_5_0060
//! doc: stdlib
//! section: 2.5
//! title: a Set of DateTime holds one entry for two values of equal Ticks and different Kinds
//! rule: A `DateTime`'s `=`, `<>`, `Equals` and `GetHashCode` read its `Ticks` alone, as
//!   `CompareTo` does: two values with equal `Ticks` are equal whatever their `Kind`s
//! cites: 2.5, 4.10
//! expect: run
//! stdout: True False 1
module Conf;

public class Program
  public static method Main()
    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc);
    var l := DateTime.FromTicks(1000L, DateTimeKind.Local);
    using var s := new Set<DateTime>();
    s.Add(u);
    var has := s.Contains(l);
    var added := s.TryAdd(l);
    Console.WriteLine($"{has} {added} {s.Count}");
  end Main;
end Program;
