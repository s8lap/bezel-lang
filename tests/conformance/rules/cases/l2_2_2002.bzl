//! case: l2_2_2002
//! doc: stdlib
//! section: 2.2
//! title: Math.Max and Math.Min over Guid separate a difference in the
//!   first byte from one in later bytes
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 2.8
//! expect: run
//! output: 01000000-0000-0000-0000-000000000000
//!   00ffffff-ffff-ffff-ffff-ffffffffffff
//!   ff000000-0000-0000-0000-000000000000
//!   7fffffff-ffff-ffff-ffff-ffffffffffff
//!   00000000-0000-0000-8000-000000000000
//!   00000000-0000-0000-7fff-ffffffffffff
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var a := Guid.Parse("01000000-0000-0000-0000-000000000000");
    var b := Guid.Parse("00ffffff-ffff-ffff-ffff-ffffffffffff");
    var p := Guid.Parse("ff000000-0000-0000-0000-000000000000");
    var q := Guid.Parse("7fffffff-ffff-ffff-ffff-ffffffffffff");
    var r := Guid.Parse("00000000-0000-0000-8000-000000000000");
    var s := Guid.Parse("00000000-0000-0000-7fff-ffffffffffff");
    Console.WriteLine(Math.Max(b, a).ToString());
    Console.WriteLine(Math.Min(a, b).ToString());
    Console.WriteLine(Math.Max(q, p).ToString());
    Console.WriteLine(Math.Min(p, q).ToString());
    Console.WriteLine(Math.Max(s, r).ToString());
    Console.WriteLine(Math.Min(r, s).ToString());
  end Main;
end Program;
