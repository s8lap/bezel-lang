//! case: l2_2_0018
//! doc: stdlib
//! section: 2.2
//! title: Clamp's supplied arguments run once each, in call-site textual order
//! rule: stdlib §2.2 declares `Clamp<T>(value: T, low: T, high: T): T where T:
//!   IComparable<T>`, and `Clamp(value, low, high)` returns `high` when the value
//!   compares greater than `high`. §22.5, Evaluation order: a call's supplied
//!   argument expressions are evaluated in call-site textual order - left to
//!   right as written, for positional and named arguments alike; naming an
//!   argument changes which parameter it binds to, never when it runs. In
//!   `Clamp(high := Next("h", 10), low := Next("l", 0), value := Next("v", 100))`
//!   the `high` expression runs first, then `low`, then `value`, each once, and
//!   the call is `Clamp(value: 100, low: 0, high: 10)`, which returns 10.
//! cites: 2.2, 22.5
//! expect: run
//! output: h
//!   l
//!   v
//!   10
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
    Console.WriteLine(Math.Clamp(high := m.Next("h", 10), low := m.Next("l", 0), value := m.Next("v", 100)).ToString());
  end Main;
end Program;
