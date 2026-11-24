//! case: s28_3_8241
//! doc: spec
//! section: 28.3
//! title: an expression-bodied method forwarding its function parameter to a function-typed one is
//!   admitted
//! rule: and every plain function-typed parameter admits a capture-bearing argument on the caller
//!   side
//! cites: 28.3
//! expect: run
//! stdout: 9
//! notes: Admitting twin of s28_3_8238 (#9045): the parameter reaches a function-typed, call-
//!   bounded parameter. Take(q) returns q() = 9.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public method M(rg: Reg2, q: () -> int): int => rg.Take(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.M(reg, lambda(): int => 9));
    return 0;
  end Main;
end Program;
