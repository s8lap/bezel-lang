//! case: s28_14_8916
//! doc: spec
//! section: 28.14
//! title: an interface implementation's indexer that returns its call-bounded index parameter is rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be returned
//! notes: Pins #9084 for the interface-implementation shape. The implementing indexer's expression body
//!   returns `q`, the call-bounded index parameter. The interface declaration itself is legal; the
//!   single error ground is the implementation's body. Twin: 8915 (a getter that does not return q).
module Conf;
public interface IIdx
  readonly property [q: () -> int]: () -> int;
end IIdx;

public class Fl: IIdx
  public constructor() end constructor;
  public property [q: () -> int]: () -> int => q;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
