//! case: s27_6_0031
//! doc: spec
//! section: 27.6
//! title: a bound reference over an interface-typed receiver captures the instance by reference
//! rule: "Method references work for static methods, instance methods (the instance is captured at
//!   formation — by reference for class- or interface-typed receivers, **by copy** for value-shaped
//!   receivers ...)" (§27.6, v0.340). The receiver `t` is interface-typed, so the formation captures
//!   the instance by reference: the two calls through `f` act on the same `Thing`, whose counter
//!   answers 1 and then 2. A capture by copy would answer 1 twice.
//! cites: 27.6
//! expect: run
//! output: 1
//!   2
module Conf;

import System;

public interface IThing
  method Bump(): int;
end IThing;

public class Thing: IThing
  private _n: int := 0;
  public method Bump(): int
    _n += 1;
    return _n;
  end Bump;
end Thing;

public class Program
  public static method Main()
    var t: IThing := new Thing();
    var f: () -> int := t.Bump;
    Console.WriteLine(f());
    Console.WriteLine(f());
  end Main;
end Program;
