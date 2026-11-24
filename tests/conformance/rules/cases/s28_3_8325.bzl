//! case: s28_3_8325
//! doc: spec
//! section: 28.3
//! title: two aliases assigned from each other, one seeded capture-free, are admitted at Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Pins #9072. Admitting twin of s28_3_8326: the assignment reaching the read of `f` is
//!   `f := g`, and the one reaching that is `var g := lambda`, capture-free; the later `g
//!   := f` assigns `g` after `f := g` and is not on any path into the read of `f`. Keep
//!   returns 1.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    var g: () -> int := lambda(): int => 9;
    f := g;
    g := f;
    return rg.Keep(f);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
