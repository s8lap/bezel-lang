//! case: s23_4_9102
//! doc: spec
//! section: 23.4
//! title: an `override` indexer with a `() -> ()` parameter is reached through the interface and
//!   the base
//! rule: They participate in `virtual`/`override`/`sealed`/`abstract` like methods.
//! cites: 11.2, 23.4, 27.1
//! expect: run
//! output: 20
//! issue: 9137
//! notes: Pins #9137 (pick 6c7cbb002): an interface indexer whose parameter is a function type with
//!   a `()` return or an inline-array return was not matched against its implementation (11.2:
//!   parameter types exactly equal under the interface's substitution), so the call crashed.
//!   The lambdas are called by the indexer bodies, so the values are exact.
module Conf;

public interface IFl
  property [q: () -> ()]: int;
end IFl;

public class Base : IFl
  public constructor() end constructor;
  public virtual property [q: () -> ()]: int
    get => 1;
    set => do end;
  end;
end Base;

public class Derived : Base
  private _v: int := 0;
  public constructor() end constructor;
  public override property [q: () -> ()]: int
    get => do q(); return _v + 7; end;
    set => do _v := value; end;
  end;
end Derived;

public class Program
  public static method Main(): int
    using var c := new Derived();
    var fl: IFl := c;
    fl[lambda() => do end] := 3;
    var t := fl[lambda() => do end];
    var b: Base := c;
    t := t + b[lambda() => do end];
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
