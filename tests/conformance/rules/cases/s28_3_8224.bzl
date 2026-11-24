//! case: s28_3_8224
//! doc: spec
//! section: 28.3
//! title: a field initializer lambda passing its function-typed parameter at a bare type parameter
//!   is rejected
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8963: `Sink<T>(x: T)` at the inferred `T = () -> int` is a set-erasing position (a
//!   parameter whose declared type is a bare type parameter, instantiated to a function
//!   type). Twin: s28_3_8225.
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
  public H: (Reg2, () -> int) -> () := lambda(rg: Reg2, q: () -> int) => Reg2.Sink(q);
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
