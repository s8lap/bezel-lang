//! case: s23_4_9001
//! doc: spec
//! section: 23.4
//! title: a virtual indexer with an empty-block setter is a no-op setter; the getter still answers
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: 0
//! notes: Issue repro of #9152 (m16_emptysetter_intindex). A virtual property accessor `set => do end;` is a
//!   setter with an empty body, like a virtual method with an empty body: calling it does nothing. Trace:
//!   the write `fl[0] := 5` runs the empty setter and returns; `_v` is still 0; `fl[0]` reads 0.
module Conf;
public class Fl
  private _v: int := 0;
  public virtual property [i: int]: int
    get => _v;
    set => do end;
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    using var fl := new Fl();
    fl[0] := 5;
    Console.WriteLine(fl[0]);
    return 0;
  end Main;
end Program;
