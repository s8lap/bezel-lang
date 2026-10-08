//! case: s22_6_0020
//! doc: spec
//! section: 22.6
//! title: a record primary constructor is a candidate at new and wins on an exact match
//! rule: Methods and constructors may be overloaded by parameter list. ... A
//!   candidate is **unambiguously better** than another when it is
//!   better-or-equal on every argument position and strictly better (an exact
//!   match where the other converts) on at least one. The call resolves to a
//!   candidate that is unambiguously better than every other applicable
//!   candidate.
//! cites: 22.6, 9.3, 11.1, 5.1
//! expect: run
//! output: 5
//!   100
module Conf;

public record R(X: int)
  public constructor(x: int?)
    X := 100;
  end constructor;
end R;

public class Program
  public static method Main()
    var a := new R(5);
    Console.WriteLine(a.X);
    var n: int? := 5;
    var b := new R(n);
    Console.WriteLine(b.X);
  end Main;
end Program;
