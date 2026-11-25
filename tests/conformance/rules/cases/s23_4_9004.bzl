//! case: s23_4_9004
//! doc: spec
//! section: 23.4
//! title: an interface indexer implemented by a virtual indexer with an empty setter: write is a no-op
//! rule: Indexers may be overloaded by parameter type list. They participate in
//!   `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 23.3, 12.5
//! expect: run
//! output: 3
//! notes: Harvested from the #9152 review (q03, with the getter given a body: the review's copy had an empty
//!   getter, which has a second error ground, #9168). Trace: the write through the `IFl` receiver
//!   dispatches to the empty setter and returns; the read returns 3.
module Conf;
public interface IFl
  property [i: int]: int;
end IFl;
public class Fl : IFl
  public virtual property [i: int]: int
    get => 3;
    set => do end;
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    using var a: IFl := new Fl();
    a[0] := 5;
    Console.WriteLine(a[0]);
    return 0;
  end Main;
end Program;
