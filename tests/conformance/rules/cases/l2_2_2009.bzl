//! case: l2_2_2009
//! doc: stdlib
//! section: 2.2
//! title: Math.Min and Math.Max over Guid pass directly to
//!   WriteLine(Object) and render the canonical form
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 2.1
//! expect: run
//! output: 11111111-1111-1111-1111-111111111111
//!   22222222-2222-2222-2222-222222222222
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var a: Array<Guid> := [Guid.Parse("22222222-2222-2222-2222-222222222222"), Guid.Parse("11111111-1111-1111-1111-111111111111")];
    Console.WriteLine(Math.Min(a[0], a[1]));
    Console.WriteLine(Math.Max(a[0], a[1]));
  end Main;
end Program;
