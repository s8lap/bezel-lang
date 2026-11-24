//! case: s28_3_8322
//! doc: spec
//! section: 28.3
//! title: a chain of three aliases whose last link keeps the stale copy of the source is
//!   rejected at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9072. Reject twin of s28_3_8321, one edit away (the final `h := g;` is
//!   removed): `h` still holds the initial `var h := g` copy of `g`, which was `f`, which
//!   was `q`, so an assignment carrying `q` reaches the read.
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
    var h: () -> int := g;
    f := lambda(): int => 9;
    g := f;
    return rg.Keep(h);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
