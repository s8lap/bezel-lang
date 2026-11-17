//! case: s14_2_4925
//! doc: spec
//! section: 14.2
//! title: a generic body over where T: IComparable<T>, IEquatable<T> admits TimeSpan: CompareTo, Equals, Sort
//! rule: Inside the generic body, T's surface includes IFoo's members, regardless of which
//!   conformance category T belongs to
//! cites: 14.2, 2.8
//! expect: run
//! output: True True True True False
//!   3 True True True
//! issue: 8578
module Conf;

public class Box<T>(v: T) where T: IComparable<T>, IEquatable<T>
  public method Bigger(o: T): T
    if v.CompareTo(o) >= 0 then return v; end if;
    return o;
  end Bigger;

  public method Same(o: T): bool
    return v.Equals(o);
  end Same;
end Box;

public class Program
  public static method Mx<T>(a: T, b: T): T where T: IComparable<T>
    if a.CompareTo(b) >= 0 then return a; end if;
    return b;
  end Mx;

  public static method Main()
    var lo := TimeSpan.FromTicks(5L);
    var hi := TimeSpan.FromTicks(900L);
    var hi2 := TimeSpan.FromTicks(950L);
    var b := new Box<TimeSpan>(lo);
    Console.WriteLine($"{Mx(lo, hi) = hi} {Mx(hi, lo) = hi} {b.Bigger(hi) = hi} {b.Same(lo)} {b.Same(hi)}");
    var xs: Array<TimeSpan> := [hi, lo, hi2];
    xs.Sort();
    Console.WriteLine($"{xs.Count} {xs[0] = lo} {xs[1] = hi} {xs[2] = hi2}");
  end Main;
end Program;
