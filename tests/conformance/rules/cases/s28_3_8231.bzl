//! case: s28_3_8231
//! doc: spec
//! section: 28.3
//! title: a field block lambda passing a local alias of its function parameter at Object is
//!   rejected
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9044: the declaration-site walk must carry flow facts for a block-bodied lambda,
//!   as the method-body twin does (provenance clause 6: a local contributes the sets of the
//!   assignments reaching the read; here `q`'s). Twin: s28_3_8236.
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
  public H: (Reg2, () -> int) -> () := lambda(rg: Reg2, q: () -> int) => do
    var f: () -> int := q;
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
