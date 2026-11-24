//! case: s28_3_8237
//! doc: spec
//! section: 28.3
//! title: a function alias forwarded to a function-typed parameter is admitted in a field lambda
//! rule: and every plain function-typed parameter admits a capture-bearing argument on the caller
//!   side
//! cites: 28.3
//! expect: run
//! stdout: 7
//! notes: Admitting twin of s28_3_8231 (#9044): the alias reaches `Take`, whose parameter is
//!   function-typed and call-bounded, not a set-erasing position. Take(f) returns f() = 7.
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
    return rg.Take(f);
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.H(reg, lambda(): int => 7));
    return 0;
  end Main;
end Program;
