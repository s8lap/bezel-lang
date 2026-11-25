//! case: s28_14_8914
//! doc: spec
//! section: 28.14
//! title: an indexer getter that returns its call-bounded index parameter through a local alias is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be returned
//! notes: Pins #9084. The index parameter `q` of an indexer is an ordinary function-typed parameter (no
//!   `scoped`), hence call-bounded; the getter binds it to a local and returns the local, which is a
//!   return of the callback. The method twin is rejected the same way; admitting twin: 8915.
module Conf;
public class Fl
  public constructor() end constructor;
  public property [q: () -> int]: () -> int
    get => do var g := q; return g; end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
