//! case: s22_6_0019
//! doc: spec
//! section: 22.6
//! title: a record primary and a body constructor that both convert at every position are ambiguous
//! rule: If no candidate is unambiguously better — because each of two
//!   candidates is exact on a different argument, or because the remaining
//!   candidates all rely on implicit conversions that this rule treats as
//!   equally ranked — the call is **ambiguous** and is a compile error. The
//!   compiler reports the error and cites the candidate overloads; it never
//!   silently selects one.
//! cites: 22.6, 9.3, 11.1, 5.1
//! expect: reject
//! diagnostic: ambiguous
//! issue: 5658
module Conf;

public record R(X: long)
  public constructor(y: double)
    X := 1;
  end constructor;
end R;

public class Program
  public static method Main()
    var r := new R(3);
    Console.WriteLine(r.X);
  end Main;
end Program;
