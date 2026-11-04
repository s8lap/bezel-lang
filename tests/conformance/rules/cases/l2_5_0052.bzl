//! case: l2_5_0052
//! doc: stdlib
//! section: 2.5
//! title: DateTime equality reads Ticks alone, whatever the Kinds
//! rule: A `DateTime`'s `=`, `<>`, `Equals` and `GetHashCode` read its `Ticks`
//!   alone, as `CompareTo` does: two values with equal `Ticks` are equal
//!   whatever their `Kind`s. `Kind` is carried and read through the `Kind`
//!   property, never compared.
//! cites: 2.5
//! expect: run
//! stdout: True False True True False
//! issue: 7764
module Conf;

public class Program
  public static method Main()
    var u := DateTime.FromTicks(1000L, DateTimeKind.Utc);
    var l := DateTime.FromTicks(1000L, DateTimeKind.Local);
    var eq := u = l;
    var ne := u <> l;
    var eqm := u.Equals(l);
    var hash := u.GetHashCode() = l.GetHashCode();
    var kinds := u.Kind = l.Kind;
    Console.WriteLine($"{eq} {ne} {eqm} {hash} {kinds}");
  end Main;
end Program;
