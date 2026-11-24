//! case: s28_3_8225
//! doc: spec
//! section: 28.3
//! title: a field initializer lambda passing a capture-free lambda at an Object parameter is
//!   admitted
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Admitting twin of s28_3_8221 (#8963): the value reaching `Keep` is capture-free
//!   (`lambda(): int => 1`), whatever the unused parameter `q` is; `Keep` returns 1.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
end Reg2;
public class Fl
  public H: (Reg2, () -> int) -> int := lambda(rg: Reg2, q: () -> int): int => rg.Keep(lambda(): int => 1);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var reg := new Reg2();
    var fl := new Fl();
    Console.WriteLine(fl.H(reg, lambda(): int => 5));
    return 0;
  end Main;
end Program;
