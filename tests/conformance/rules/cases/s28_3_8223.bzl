//! case: s28_3_8223
//! doc: spec
//! section: 28.3
//! title: an expression-bodied method returning a lambda that passes its fn parameter at Object is
//!   rejected
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #8963 for the `=>` method body, a declaration-site expression with no statements.
//!   Twin: s28_3_8225.
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
  public constructor() end constructor;
  public method M(): (Reg2, () -> int) -> () => lambda(rg: Reg2, q: () -> int) => rg.Keep(q);
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
