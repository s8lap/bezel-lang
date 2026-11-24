//! case: s28_3_8226
//! doc: spec
//! section: 28.3
//! title: a field lambda forwarding its function parameter to a function-typed parameter is
//!   admitted
//! rule: and every plain function-typed parameter admits a capture-bearing argument on the caller
//!   side
//! cites: 28.3
//! expect: run
//! stdout: 42
//! notes: Admitting twin of s28_3_8221 (#8963): `Take` declares a function type, a call-bounded
//!   parameter, so the capturing argument (and the forwarded `q`) are admitted. Take(q)
//!   returns q(), which is k + 1 = 42.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public H: (Reg2, () -> int) -> int := lambda(rg: Reg2, q: () -> int): int => rg.Take(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    var k := 41;
    Console.WriteLine(fl.H(reg, lambda(): int => k + 1));
    return 0;
  end Main;
end Program;
