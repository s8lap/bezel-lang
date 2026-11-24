//! case: s28_3_8236
//! doc: spec
//! section: 28.3
//! title: a function alias overwritten with a capture-free lambda before its Object use is
//!   admitted
//! rule: contributes, at each read, the union of the sets of the forms the flow analysis traces
//!   it to, over every assignment that reaches the read
//! cites: 28.3
//! expect: run
//! stdout: 4
//! notes: Admitting twin of s28_3_8231 and s28_3_8233 (#9044): the only assignment reaching the
//!   read at `Keep` is the capture-free lambda, so `q` does not reach it. Keep returns 1 and
//!   f() returns 3, so the result is 4. (The method-body spelling of this program is the
//!   over-rejection #9059; only the declaration-site spelling is in this case.)
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public H: (Reg2, () -> int) -> int := lambda(rg: Reg2, q: () -> int): int => do
    var f: () -> int := q;
    f := lambda(): int => 3;
    var n := rg.Keep(f);
    return n + f();
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.H(reg, lambda(): int => 100));
    return 0;
  end Main;
end Program;
