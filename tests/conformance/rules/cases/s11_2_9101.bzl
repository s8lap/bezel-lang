//! case: s11_2_9101
//! doc: spec
//! section: 11.2
//! title: a generic interface `IIdx<int>` with a `() -> ()` indexer parameter is implemented and
//!   called
//! rule: parameter types that are exactly equal under the interface's substitution
//! cites: 11.2, 23.4, 27.1
//! expect: run
//! output: 3
//! issue: 9137
//! notes: Pins #9137 (pick 6c7cbb002): an interface indexer whose parameter is a function type with
//!   a `()` return or an inline-array return was not matched against its implementation (11.2:
//!   parameter types exactly equal under the interface's substitution), so the call crashed.
//!   The lambdas are called by the indexer bodies, so the values are exact.
module Conf;

public interface IIdx<T>
  property [q: () -> ()]: T;
end IIdx;

public class Impl : IIdx<int>
  private _v: int := 0;
  public property [q: () -> ()]: int
    get => do q(); return _v + 1; end;
    set => do _v := value; end;
  end;
  public constructor() end constructor;
end Impl;

public class Program
  public static method Main(): int
    using var c := new Impl();
    var fl: IIdx<int> := c;
    fl[lambda() => do end] := 2;
    Console.WriteLine(fl[lambda() => do end]);
    return 0;
  end Main;
end Program;
