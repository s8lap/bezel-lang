//! case: s28_3_8316
//! doc: spec
//! section: 28.3
//! title: an indexer forwarding its function parameter to a function-typed parameter is
//!   admitted
//! rule: and every plain function-typed parameter admits a capture-bearing argument on the
//!   caller side
//! cites: 28.3, 23.4
//! expect: run
//! stdout: 7
//! notes: Admitting twin of s28_3_8311..8315 (#9063): the parameter reaches a function-typed,
//!   call-bounded parameter, not an Object one, so the capture-bearing lambda at the call
//!   site `fl[reg, lambda(): int => n]` is admitted. Take returns q() = n = 7.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Note(o: Object) end Note;
  public method Take(f: () -> int): int return f(); end Take;
  public method Run(f: () -> int) end Run;
end Reg2;
public class Fl
  public property [rg: Reg2, q: () -> int]: int => rg.Take(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    var n := 7;
    Console.WriteLine(fl[reg, lambda(): int => n]);
    return 0;
  end Main;
end Program;
