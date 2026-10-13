//! case: s28_14_0026
//! doc: spec
//! section: 28.14
//! title: another body invoking a public scoped field through a reference is rejected
//! rule: A `scoped` **field** holds such a callback: it may be **invoked** — by
//!   its declaring type's own member bodies only, through `this` (`_cb(x)`,
//!   `this._cb(x)`, a lambda in such a body included); a derived class's
//!   bodies, a nested type's ... and every other body may not invoke it,
//!   through `this` or another reference, and a type that wants the
//!   invocation reachable exposes a method that performs it
//! cites: 28.14
//! expect: reject
//! diagnostic: scoped
//! issue: 5904
module Conf;

public class Ticker
  public scoped _cb: (int) -> int;

  public constructor(scoped cb: (int) -> int)
    _cb := cb;
  end constructor;

  public method Fire(x: int): int => _cb(x);
end Ticker;

public class Program
  public static method Main()
    using var t := new Ticker(lambda(x) => x * 2);
    Console.WriteLine(t._cb(10));
  end Main;
end Program;
