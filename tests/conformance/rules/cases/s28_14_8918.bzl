//! case: s28_14_8918
//! doc: spec
//! section: 28.14
//! title: an indexer getter that returns a closure capturing its call-bounded index parameter is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be returned
//! notes: Pins #9084. The returned lambda captures `q`, so the closure that escapes the callee carries the
//!   call-bounded callback ('captured by a closure that itself escapes the callee'). Twin: 8915 returns
//!   a lambda that does not mention q.
module Conf;
public class Fl
  public constructor() end constructor;
  public property [q: () -> int]: () -> int => lambda() => q();
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
