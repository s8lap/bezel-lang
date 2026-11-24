//! case: s28_3_8307
//! doc: spec
//! section: 28.3
//! title: an alias overwritten in every arm of an if, elif and else chain is admitted at Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Pins #9059. Admitting twin of s28_3_8308: the chain has an else, so every path to the
//!   read passes through an arm and every arm assigns a capture-free lambda to `f`; no
//!   assignment carrying `q` reaches the read. Keep returns 1.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    if c then
      f := lambda(): int => 9;
    elif not c then
      f := lambda(): int => 9;
    else
      f := lambda(): int => 9;
    end if;
    return rg.Keep(f);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
