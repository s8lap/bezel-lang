//! case: s31_1_7700
//! doc: spec
//! section: 31.1
//! title: a conditional throw operand with a borrowed arm is rejected inside a closure
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 28.3
//! expect: reject
//! diagnostic: owned exception
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var o := new R();
      var b: R := o;
      var n := 1;
      throw (if n > 0 then b else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
