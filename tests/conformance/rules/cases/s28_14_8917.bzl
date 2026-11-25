//! case: s28_14_8917
//! doc: spec
//! section: 28.14
//! title: an overriding indexer that returns its call-bounded index parameter is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be returned
//! notes: Pins #9084 for the override shape. The base indexer returns a capture-free closure (legal); the
//!   override returns its call-bounded parameter `q`, rejected. Twin: 8915.
module Conf;
public class Base
  public constructor() end constructor;
  public virtual property [q: () -> int]: () -> int => lambda() => 1;
end Base;

public class Fl: Base
  public constructor() end constructor;
  public override property [q: () -> int]: () -> int => q;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
