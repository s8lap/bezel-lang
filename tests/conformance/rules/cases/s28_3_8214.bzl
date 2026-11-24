//! case: s28_3_8214
//! doc: spec
//! section: 28.3
//! title: a captured owned local in a conditional nested in a thrown case arm is rejected
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9043: the case walker and the conditional walker compose. Twin: s28_3_8215.
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
      throw (case n of 1 => (if n > 0 then c else new R()), _ => new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
