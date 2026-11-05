//! case: l2_2_2005
//! doc: stdlib
//! section: 2.2
//! title: Math.Max and Math.Clamp inside a generic method constrained
//!   IComparable instantiate at Guid
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 14.2
//! expect: run
//! output: 80000000-0000-0000-0000-000000000000
//!   80000000-0000-0000-0000-000000000000
//!   7fffffff-ffff-ffff-ffff-ffffffffffff
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Program
  public static method Greater<T>(a: T, b: T): T where T: IComparable<T>
    return Math.Max(a, b);
  end Greater;

  public static method Bounded<T>(v: T, lo: T, hi: T): T where T: IComparable<T>
    return Math.Clamp(v, lo, hi);
  end Bounded;

  public static method Main()
    var big := Guid.Parse("80000000-0000-0000-0000-000000000000");
    var small := Guid.Parse("7fffffff-ffff-ffff-ffff-ffffffffffff");
    Console.WriteLine(Greater<Guid>(big, small).ToString());
    Console.WriteLine(Greater<Guid>(small, big).ToString());
    Console.WriteLine(Bounded<Guid>(big, Guid.Empty, small).ToString());
  end Main;
end Program;
