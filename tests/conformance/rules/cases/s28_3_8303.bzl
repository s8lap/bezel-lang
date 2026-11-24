//! case: s28_3_8303
//! doc: spec
//! section: 28.3
//! title: an alias overwritten by a conditional of two capture-free arms is admitted at Object
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Pins #9059. Admitting twin of s28_3_8304: the only assignment reaching the read is
//!   the conditional expression, whose two arms are capture-free lambdas, so the set is
//!   empty. Keep returns 1.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
end Reg2;
public class Program
  public static method Via(rg: Reg2, q: () -> int, c: bool): int
    var f: () -> int := q;
    f := (if c then lambda() => 9 else lambda() => 9);
    return rg.Keep(f);
  end Via;
  public static method Main(): int
    var reg := new Reg2();
    var n := 5;
    Console.WriteLine(Program.Via(reg, lambda(): int => n, true));
    return 0;
  end Main;
end Program;
