//! case: s27_7_1_0008
//! doc: spec
//! section: 27.7.1
//! title: two method references over an interface-typed receiver are equal
//! rule: "A method reference equals another method reference iff both denote the
//!   same method and, for instance references, are bound to the same receiver —
//!   receiver sameness being reference identity for class-typed receivers"
//!   (§27.7.1, v0.340). The head `t` is interface-typed and holds one class
//!   instance, so the two evaluations of `t.Bump` denote the same method and bind
//!   the same receiver: the two function values are equal.
//! cites: 27.7.1, 27.6
//! expect: run
//! stdout: True
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
    var g: () -> int := t.Bump;
    Console.WriteLine(f = g);
  end Main;
end Program;
