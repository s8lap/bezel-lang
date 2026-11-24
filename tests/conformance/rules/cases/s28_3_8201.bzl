//! case: s28_3_8201
//! doc: spec
//! section: 28.3
//! title: a closure throwing a conditional of two captured owned local arms is rejected
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #8958. Inside the closure `c` is a captured owned binding, in borrow state; both
//!   arms are unmarked reads of it, so the conditional is a borrow and `throw` would consume
//!   it, which §28.3 rejects (and §31.1 states a borrowed class reference is no valid
//!   operand). Twin: s28_3_8205 (fresh arms).
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
      throw (if n > 0 then c else c);
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
