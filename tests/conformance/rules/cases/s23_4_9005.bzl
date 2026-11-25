//! case: s23_4_9005
//! doc: spec
//! section: 23.4
//! title: compound assignment through a virtual indexer with an empty setter leaves the value unchanged
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: 3
//! notes: Harvested from the #9152 review (q10). Trace: `fl[0] += 5` reads 3, adds 5 and calls the empty
//!   setter (a no-op); `fl[0] := fl[0] + 1` likewise; the final read is 3 (the getter is the constant 3).
module Conf;
public class Fl
  public virtual property [i: int]: int
    get => 3;
    set => do end;
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    fl[0] += 5;
    fl[0] := fl[0] + 1;
    Console.WriteLine(fl[0]);
    return 0;
  end Main;
end Program;
