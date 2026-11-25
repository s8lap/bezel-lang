//! case: s11_2_9102
//! doc: spec
//! section: 11.2
//! title: a field closure, a method group and a static method group are `() -> ()` indexer
//!   arguments
//! rule: parameter types that are exactly equal under the interface's substitution
//! cites: 11.2, 23.4, 27.1
//! expect: run
//! output: 6
//! issue: 9137
//! notes: Pins #9137 (pick 6c7cbb002): an interface indexer whose parameter is a function type with
//!   a `()` return or an inline-array return was not matched against its implementation (11.2:
//!   parameter types exactly equal under the interface's substitution), so the call crashed.
//!   The lambdas are called by the indexer bodies, so the values are exact.
module Conf;

public interface IFl
  property [q: () -> ()]: int;
end IFl;

public class FlI : IFl
  private _v: int := 0;
  public constructor() end constructor;
  public property [q: () -> ()]: int
    get => do q(); return _v; end;
    set => do q(); _v := value; end;
  end;
end FlI;

public class Holder
  public cb: () -> () := lambda() => do end;
  public constructor() end constructor;
  public method Tick()
  end Tick;
end Holder;

public class Program
  public static method Noop()
  end Noop;

  public static method Main(): int
    using var c := new FlI();
    using var h := new Holder();
    var fl: IFl := c;
    fl[h.cb] := 2;
    var t := fl[h.cb];
    t := t + fl[h.Tick];
    t := t + fl[Program.Noop];
    Console.WriteLine(t);
    return 0;
  end Main;
end Program;
