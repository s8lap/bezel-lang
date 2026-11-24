//! case: s28_3_8235
//! doc: spec
//! section: 28.3
//! title: a function alias declared in a branch and passed at Object is rejected in a field lambda
//! rule: At every set-erasing position a function value with a non-empty source set is rejected,
//!   and a capture-free one is admitted
//! cites: 28.3
//! expect: reject
//! diagnostic: capture-free
//! notes: Pins #9044: the local is declared and read inside the `if` branch; `g`'s only assignment
//!   is `q`. Twin: s28_3_8236.
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
    if c then var g: () -> int := q; rg.Keep(g); end if;
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
