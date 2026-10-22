//! case: l2_2_0014
//! doc: stdlib
//! section: 2.2
//! title: Max, Min and Clamp in a generic body order uint and byte arguments by their unsigned CompareTo
//! rule: Ordering throughout is T's CompareTo (§2.8). Min(a, b) returns a when
//!   a.CompareTo(b) <= 0 and b otherwise; Max(a, b) returns a when
//!   a.CompareTo(b) >= 0 and b otherwise.
//! cites: 2.2
//! expect: run
//! output: 4000000000
//!   3
//!   5
//!   200
//! issue: 6908
module Conf;

public static class Util
  public static method Big<T>(a: T, b: T): T where T: IComparable<T>
    return Math.Max(a, b);
  end Big;
  public static method Small<T>(a: T, b: T): T where T: IComparable<T>
    return Math.Min(a, b);
  end Small;
  public static method Fit<T>(v: T, lo: T, hi: T): T where T: IComparable<T>
    return Math.Clamp(v, lo, hi);
  end Fit;
end Util;

public class Program
  public static method Main()
    Console.WriteLine(Util.Big<uint>(3u, 4000000000u));
    Console.WriteLine(Util.Small<uint>(3u, 4000000000u));
    Console.WriteLine(Util.Fit<uint>(4000000000u, 3u, 5u));
    Console.WriteLine(Util.Big<byte>(1, 200));
  end Main;
end Program;
