//! case: s28_3_8240
//! doc: spec
//! section: 28.3
//! title: an expression-bodied method passing its own function parameter at a bare type parameter
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9045: `Sink<() -> int>` declares its parameter at a bare type parameter
//!   instantiated to a function type, a set-erasing position. Twin: s28_3_8241.
module Conf;
import System;
public class Reg2
  private _o: Object? := null;
  public constructor() end constructor;
  public method Keep(o: Object) _o := o; end Keep;
  public method Take(f: () -> int): int return f(); end Take;
  public static method Sink<T>(x: T) end Sink;
end Reg2;
public class Fl
  public method M(q: () -> int) => Reg2.Sink<() -> int>(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
