//! case: s28_3_8312
//! doc: spec
//! section: 28.3
//! title: an indexer `get =>` passing its own function parameter at an Object parameter is
//!   rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.4
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9063 for the `get =>` accessor. The only set-erasing site is the getter's
//!   `rg.Keep(q)`; the setter forwards `q` to a function-typed parameter, which is not a
//!   set-erasing position. Twin: s28_3_8316.
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
  public property [rg: Reg2, q: () -> int]: int
    get => rg.Keep(q);
    set => rg.Run(q);
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
