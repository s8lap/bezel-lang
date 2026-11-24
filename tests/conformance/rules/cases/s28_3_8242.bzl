//! case: s28_3_8242
//! doc: spec
//! section: 28.3
//! title: an expression-bodied method invoking its own function-typed parameter is admitted
//! rule: it may be immediately invoked
//! cites: 28.3
//! expect: run
//! stdout: 10
//! notes: Over-rejection guard for #9045: invoking a function-typed parameter is no store and no
//!   set-erasing position. q() + 1 = 10.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public method M(q: () -> int): int => q() + 1;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.M(lambda(): int => 9));
    return 0;
  end Main;
end Program;
