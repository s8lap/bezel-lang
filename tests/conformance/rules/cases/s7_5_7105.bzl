//! case: s7_5_7105
//! doc: spec
//! section: 7.5
//! title: `?.First` / `?.Last` on LinkedList of a record, a double and a bool, present and null receivers
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 7.5, 7.7
//! expect: run
//! output: 1
//!   4
//!   2.5
//!   True
//!   -1
//!   -1
//!   -1.5
//!   False
module Conf;
public record Pt(X: int, Y: int);
public class Program
  public static method F(m: LinkedList<Pt>?, d: LinkedList<double>?, b: LinkedList<bool>?)
    var p := m?.First;
    Console.WriteLine(p?.X ?? -1);
    Console.WriteLine(m?.Last?.Y ?? -1);
    Console.WriteLine(d?.First ?? -1.5);
    Console.WriteLine(b?.Last ?? false);
  end F;
  public static method Main()
    using var l := new LinkedList<Pt>();
    _ := l.AddLast(new Pt(1, 2));
    _ := l.AddLast(new Pt(3, 4));
    using var d := new LinkedList<double>();
    _ := d.AddLast(2.5);
    using var b := new LinkedList<bool>();
    _ := b.AddLast(true);
    F(l, d, b);
    F(null, null, null);
  end Main;
end Program;
