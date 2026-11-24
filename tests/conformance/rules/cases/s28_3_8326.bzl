//! case: s28_3_8326
//! doc: spec
//! section: 28.3
//! title: two aliases assigned from each other, the second seeded from the first, are rejected
//!   at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9072. Reject twin of s28_3_8325 (the second alias is seeded from `f` and the
//!   read is of `g`): `g` is `f`, which is `q`; `f := g` copies `q`, and `g := f` copies
//!   it again, so every assignment reaching the read of `g` carries `q`.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    var g: () -> int := f;
    f := g;
    g := f;
    return rg.Keep(g);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
