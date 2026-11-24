//! case: s28_3_8321
//! doc: spec
//! section: 28.3
//! title: a chain of three aliases, each reassigned after the source is overwritten, is
//!   admitted at Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Pins #9072. Admitting twin of s28_3_8322: the reaching assignment of `h` is `h := g`,
//!   and the one reaching that is `g := f`, and the one reaching that is `f := lambda`,
//!   capture-free, so the chain traces to no assignment carrying `q`. Keep returns 1.
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
    h := g;
    return rg.Keep(h);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
