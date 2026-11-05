//! case: l2_2_2001
//! doc: stdlib
//! section: 2.2
//! title: Math.Max and Math.Min over Guid order by the last byte and treat
//!   0x80 above 0x7f
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 2.8
//! expect: run
//! output: 00000000-0000-0000-0000-000000000080
//!   00000000-0000-0000-0000-00000000007f
//!   00000000-0000-0000-0000-000000000001
//!   00000000-0000-0000-0000-000000000000
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var a := Guid.Parse("00000000-0000-0000-0000-000000000080");
    var b := Guid.Parse("00000000-0000-0000-0000-00000000007f");
    var c := Guid.Parse("00000000-0000-0000-0000-000000000001");
    var z := Guid.Parse("00000000-0000-0000-0000-000000000000");
    Console.WriteLine(Math.Max(a, b).ToString());
    Console.WriteLine(Math.Min(a, b).ToString());
    Console.WriteLine(Math.Max(z, c).ToString());
    Console.WriteLine(Math.Min(c, z).ToString());
  end Main;
end Program;
