//! case: s28_3_8314
//! doc: spec
//! section: 28.3
//! title: a generic class's indexer passing its own function parameter at an Object parameter
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.4
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9063 for a class declared generic: the indexer's own parameter provenance does
//!   not depend on the enclosing type's parameters. Twin: s28_3_8316.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Note(o: Object) end Note;
  public method Take(f: () -> int): int return f(); end Take;
  public method Run(f: () -> int) end Run;
end Reg2;
public class Fl<T>
  public property [rg: Reg2, q: () -> int]: int => rg.Keep(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
