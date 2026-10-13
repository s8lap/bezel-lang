//! case: s27_6_0012
//! doc: spec
//! section: 27.6
//! title: a generic method reference at an argument meeting its constraints is formed and invoked
//! rule: The formation is an **instantiation site** (§14.1): §3.6 rule 2's
//!   requirements, §14.2's constraints, its dispatched-method exclusion
//!   (§14.2) and §28.3's two per-instantiation exclusions ... are read at the
//!   formation exactly as at a call with that list, and never at an
//!   invocation through the formed value, which reads the function type alone.
//! cites: 27.6, 14.1, 14.2
//! expect: run
//! stdout: 0
//! issue: 5814
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Util
  public static method Order<T>(a: T, b: T): int where T: IComparable<T>
    return a.CompareTo(b);
  end Order;

  public static method Main()
    var f: (int, int) -> int := Util.Order<int>;
    Console.WriteLine(f(3, 3));
  end Main;
end Util;
