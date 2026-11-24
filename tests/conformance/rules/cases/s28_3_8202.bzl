//! case: s28_3_8202
//! doc: spec
//! section: 28.3
//! title: a closure throwing a conditional with a fresh arm and a captured owned arm is rejected
//! rule: a closure cannot consume, reassign, or dispose a captured owned binding
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: captured owned binding
//! notes: Pins #8958. One arm is fresh and legal, the other is the captured owned binding; the arm
//!   that reads `c` is the consume the rule rejects wherever it sits. Twin: s28_3_8205.
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
      throw (if n > 0 then new R() else c);
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
