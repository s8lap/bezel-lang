//! case: s28_3_8306
//! doc: spec
//! section: 28.3
//! title: an alias overwritten by a coalesce whose right operand is `q` is rejected at Object
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9059. Reject twin of s28_3_8305, one edit away (the right operand is `q`): the
//!   coalesce traces to its right operand, a plain function-typed parameter whose set is
//!   non-empty (§28.3 provenance clause 3).
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    var m: (() -> int)? := null;
    f := m ?? q;
    return rg.Keep(f);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
