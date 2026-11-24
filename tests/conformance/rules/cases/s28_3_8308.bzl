//! case: s28_3_8308
//! doc: spec
//! section: 28.3
//! title: an alias overwritten only in a nested arm of the else of an if is rejected at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9059. Reject twin of s28_3_8307: the path where the outer condition is false
//!   and the inner condition is false assigns nothing, so the initial assignment `f := q`
//!   reaches the read on that path, and the set is non-empty (§28.3 provenance clause 3).
//!   A flow analysis that treated the else arm's nested if as killing would admit it.
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
    else
      if c then
        f := lambda(): int => 9;
      end if;
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
