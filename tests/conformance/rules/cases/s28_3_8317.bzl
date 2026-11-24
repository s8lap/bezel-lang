//! case: s28_3_8317
//! doc: spec
//! section: 28.3
//! title: an expression-bodied indexer that only invokes its function parameter is admitted
//! rule: and every plain function-typed parameter admits a capture-bearing argument on the
//!   caller side
//! cites: 28.3, 23.4
//! expect: run
//! stdout: 7
//! notes: Admitting twin of s28_3_8311 (#9063): the parameter is only invoked, never passed at
//!   an Object parameter. The call `fl[lambda(): int => n]` yields n = 7.
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
  public property [q: () -> int]: int => q();
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    var n := 7;
    Console.WriteLine(fl[lambda(): int => n]);
    return 0;
  end Main;
end Program;
