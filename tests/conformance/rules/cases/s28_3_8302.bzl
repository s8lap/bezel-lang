//! case: s28_3_8302
//! doc: spec
//! section: 28.3
//! title: an alias of a call-bounded parameter read in a loop pass before its overwrite is
//!   rejected at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9059. Reject twin of s28_3_8301, one edit away (read and overwrite swapped): in
//!   the first pass `f` still holds `q` at the read, so an assignment carrying `q` reaches
//!   it; `q` is a plain function-typed parameter (§28.3 provenance clause 3), so the set
//!   is non-empty at the Object parameter.
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
      t := t + rg.Keep(f);
      f := lambda(): int => 9;
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
