//! case: s28_3_8203
//! doc: spec
//! section: 28.3
//! title: a captured owned arm deep in an elif chain with a nested conditional is rejected
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #8958's recursion: the conditional walker must descend into elif arms and
//!   parenthesised nested conditionals. Only the innermost else arm names `c`. Twin:
//!   s28_3_8205.
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
      throw (if n > 1 then new R() elif n > 0 then (if n > 5 then new R() else c) else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
