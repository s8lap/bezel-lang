//! case: l2_2_2006
//! doc: stdlib
//! section: 2.2
//! title: Math.Max on a field of a class generic over an IComparable
//!   parameter keeps the greatest Guid
//! rule: `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b`
//!   otherwise; `Max(a, b)` returns `a` when `a.CompareTo(b) >= 0` and
//!   `b` otherwise
//! cites: 2.6, 14.2
//! expect: run
//! output: 80000000-0000-0000-0000-000000000000
//! notes: stdlib 2.6: Guid CompareTo is lexicographic over the 16 canonical
//!   big-endian bytes (the textual hex order), unsigned. Failed on the
//!   compiler before the #7935 fix and passes on the staging tip.
module Conf;

public class Keeper<T> where T: IComparable<T>
  private best: T;

  public constructor(first: T)
    best := first;
  end constructor;

  public method Offer(x: T)
    best := Math.Max(best, x);
  end Offer;

  public method Best(): T
    return best;
  end Best;
end Keeper;

public class Program
  public static method Main()
    using var k := new Keeper<Guid>(Guid.Parse("7fffffff-ffff-ffff-ffff-ffffffffffff"));
    k.Offer(Guid.Parse("80000000-0000-0000-0000-000000000000"));
    k.Offer(Guid.Parse("10000000-0000-0000-0000-000000000000"));
    Console.WriteLine(k.Best().ToString());
  end Main;
end Program;
