//! case: s28_3_8232
//! doc: spec
//! section: 28.3
//! title: a function alias assigned in a loop and passed at Object after it is rejected in a field
//!   lambda
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9044: the loop body may run, so `f := q` reaches the read after the loop (clause
//!   6, union over reaching assignments). Twin: s28_3_8236.
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
  public H: (Reg2, () -> int, bool) -> () := lambda(rg: Reg2, q: () -> int, c: bool) => do
    var f: () -> int := lambda() => 9;
    for var i in 0 ..< 3 do f := q; end for;
    rg.Keep(f);
  end;
  public constructor() end constructor;
end Fl;
public class Program
  public static method Main(): int
    var fl := new Fl();
    Console.WriteLine("ok");
    return 0;
  end Main;
end Program;
