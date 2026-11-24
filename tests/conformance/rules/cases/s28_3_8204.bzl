//! case: s28_3_8204
//! doc: spec
//! section: 28.3
//! title: a closure throwing a conditional with a `move` arm over a captured owned local is
//!   rejected
//! rule: a marked transfer from it, an explicit `Dispose()` or discard of it, and every other
//!   owned-consuming use of it are compile errors
//! cites: 28.3, 28.8.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Boundary of #8958's arm walk: a marked transfer from a captured owned binding is
//!   rejected by §28.3 on its own, with or without the conditional. Twin: s28_3_8208 (the
//!   binding is moved out before the closure is formed, so the closure captures nothing
//!   owned).
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
  public method Flag(): bool return true; end Flag;
  public method Code(): int return 1; end Code;
  public method Make(): owned R return new R(); end Make;
end R;
public class Program
  public static method Main(): int
    var c := new R(); var n := 1;
    var f := lambda(): int => do
      throw (if n > 0 then move c else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
