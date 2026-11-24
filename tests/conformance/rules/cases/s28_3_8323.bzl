//! case: s28_3_8323
//! doc: spec
//! section: 28.3
//! title: a chain of six aliases all reassigned after the source is overwritten is admitted at
//!   Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Pins #9072. Admitting twin of s28_3_8324: each link is reassigned from the previous
//!   one after the capture-free overwrite of `f0`, so the final `f5` traces to the lambda
//!   alone. Keep returns 1.
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
    f2 := f1;
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
