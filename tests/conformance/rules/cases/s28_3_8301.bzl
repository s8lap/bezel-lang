//! case: s28_3_8301
//! doc: spec
//! section: 28.3
//! title: an alias overwritten before the read in each loop pass is admitted at Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 2
//! notes: Pins #9059. Admitting twin of s28_3_8302: `f` starts as `q` but the capture-free
//!   overwrite precedes the read in every pass, on the first entry and on the back edge,
//!   so no assignment carrying `q` reaches `rg.Keep(f)`. Keep returns 1 per call, two
//!   passes, so Via returns 2.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    var t := 0;
    for var i in 0 ..< 2 do
      f := lambda(): int => 9;
      t := t + rg.Keep(f);
    end for;
    return t;
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
