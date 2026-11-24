//! case: s28_3_8315
//! doc: spec
//! section: 28.3
//! title: a record's indexer passing its own function parameter at an Object parameter is
//!   rejected
//! rule: At every set-erasing position a function value with a non-empty source set is
//!   rejected, and a capture-free one is admitted
//! cites: 28.3, 23.4
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9063 for a record's read-only indexer (§23.4: records may declare read-only
//!   indexers). Twin: s28_3_8316.
module Conf;
import System;
public class Reg2
  public constructor() end constructor;
  public method Keep(o: Object): int return 1; end Keep;
  public method Note(o: Object) end Note;
  public method Take(f: () -> int): int return f(); end Take;
  public method Run(f: () -> int) end Run;
end Reg2;
public record Fl(X: int)
  public property [rg: Reg2, q: () -> int]: int => rg.Keep(q);
end Fl;
public class Program
  public static method Main(): int
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
