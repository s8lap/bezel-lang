//! case: s28_3_8212
//! doc: spec
//! section: 28.3
//! title: a captured owned local in only the last arm of a thrown case expression is rejected
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #9043: the walker must reach every arm, not only the first. Twin: s28_3_8215.
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
      throw (case n of 1 => new R(), _ => c);
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
