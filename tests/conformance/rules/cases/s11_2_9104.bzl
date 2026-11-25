//! case: s11_2_9104
//! doc: spec
//! section: 11.2
//! title: a method twin of the `() -> ()` and `() -> [3]int` parameters is matched and called
//! rule: parameter types that are exactly equal under the interface's substitution
//! cites: 11.2, 23.4, 27.1
//! expect: run
//! output: 5
//! issue: 9137
//! notes: Pins #9137 (pick 6c7cbb002): an interface indexer whose parameter is a function type with
//!   a `()` return or an inline-array return was not matched against its implementation (11.2:
//!   parameter types exactly equal under the interface's substitution), so the call crashed.
//!   The lambdas are called by the indexer bodies, so the values are exact. Spec-decided
//!   control (a method twin, not an indexer): green on both arms.
module Conf;

public interface IM
  method Run(q: () -> ()): int;
  method Arr(q: () -> [3]int): int;
end IM;

public class Impl : IM
  public constructor() end constructor;
  public method Run(q: () -> ()): int
    q();
    return 3;
  end Run;
  public method Arr(q: () -> [3]int): int
    var a := q();
    return a[1];
  end Arr;
end Impl;

public class Program
  public static method Main(): int
    using var c := new Impl();
    var m: IM := c;
    var t := m.Run(lambda() => do end);
    t := t + m.Arr(lambda() => do var a: [3]int := [1, 2, 3]; return a; end);
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
