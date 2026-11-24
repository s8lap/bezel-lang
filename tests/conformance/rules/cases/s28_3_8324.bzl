//! case: s28_3_8324
//! doc: spec
//! section: 28.3
//! title: a chain of six aliases with one link left stale is rejected at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9072. Reject twin of s28_3_8323, one edit away (`f2 := f1;` is removed): `f2`
//!   keeps its initial copy of `f1`, which was `q`, so `f3 := f2` re-introduces `q` down
//!   the chain to `f5`.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f0: () -> int := q;
    var f1: () -> int := f0;
    var f2: () -> int := f1;
    var f3: () -> int := f2;
    var f4: () -> int := f3;
    var f5: () -> int := f4;
    f0 := lambda(): int => 9;
    f1 := f0;
    f3 := f2;
    f4 := f3;
    f5 := f4;
    return rg.Keep(f5);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
