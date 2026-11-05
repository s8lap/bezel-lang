//! case: l2_8_2010
//! doc: stdlib
//! section: 2.8
//! title: Math.Max over Guid agrees with CompareTo: the Guid that compares
//!   greater is returned
//! rule: `a.CompareTo(b)` returns a negative number if `a < b`, zero if
//!   equal, a positive number if `a > b`.
//! cites: 2.2, 2.6
//! expect: run
//! output: 80000000-0000-0000-0000-000000000000
//!   7fffffff-ffff-ffff-ffff-ffffffffffff
//!   True
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Main()
    var a: Array<Guid> := [Guid.Parse("80000000-0000-0000-0000-000000000000"), Guid.Parse("7fffffff-ffff-ffff-ffff-ffffffffffff"), Guid.Parse("00000000-0000-0000-0000-000000000001")];
    Console.WriteLine(Math.Max(a[0], a[1]).ToString());
    Console.WriteLine(Math.Clamp(a[0], a[2], a[1]).ToString());
    Console.WriteLine(a[0].CompareTo(a[1]) > 0);
  end Main;
end Program;
