//! case: s28_14_8922
//! doc: spec
//! section: 28.14
//! title: an indexer getter that forwards its call-bounded index parameter to a `scoped` parameter is
//!   rejected
//! rule: returned, captured by a closure that itself escapes the callee (judged by these same rules,
//!   applied transitively), or forwarded into a `scoped` parameter position
//! cites: 28.14, 23.4, 23.3
//! expect: reject
//! diagnostic: may not be forwarded into a `scoped` parameter position
//! notes: Pins #9084. `q` is call-bounded; `new Sink3(q)` forwards it into a `scoped` parameter position,
//!   which the callee cannot vouch for. The method twin (`method Mt(q: () -> ())` doing the same) is
//!   rejected identically. Twin: 8924 forwards q only to a plain call-bounded parameter.
module Conf;
public class Sink3
  private scoped _cb: (() -> ())? := null;
  public constructor(scoped cb: () -> ()) _cb := cb; end constructor;
end Sink3;

public class Fl
  public constructor() end constructor;
  public property [q: () -> ()]: int
    get => do var s := new Sink3(q); return 0; end;
  end;
end Fl;

public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
