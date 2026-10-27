//! case: l2_2_0017
//! doc: stdlib
//! section: 2.2
//! title: Min's supplied arguments run once each, in call-site textual order
//! rule: stdlib §2.2 declares `Min<T>(a: T, b: T): T where T: IComparable<T>`, and
//!   `Min(a, b)` returns `a` when `a.CompareTo(b) <= 0` and `b` otherwise. §22.5,
//!   Evaluation order: a call's supplied argument expressions are evaluated in
//!   call-site textual order - left to right as written, for positional and named
//!   arguments alike; naming an argument changes which parameter it binds to,
//!   never when it runs. In `Min(b := Next("b", 2), a := Next("a", 1))` the `b`
//!   expression runs first, the `a` expression second, each once, and the call is
//!   `Min(a: 1, b: 2)`, which returns 1.
//! cites: 2.2, 22.5
//! expect: run
//! output: b
//!   a
//!   1
//! issue: 7219
module Conf;

public class Marker
  public method Next(tag: string, value: int): int
    Console.WriteLine(tag);
    return value;
  end Next;
end Marker;

public class Program
  public static method Main()
    var m := new Marker();
    Console.WriteLine(Math.Min(b := m.Next("b", 2), a := m.Next("a", 1)).ToString());
  end Main;
end Program;
