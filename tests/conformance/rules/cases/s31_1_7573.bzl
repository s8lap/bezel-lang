//! case: s31_1_7573
//! doc: spec
//! section: 31.1
//! title: a throw of a borrow bound in an enclosing if block from a nested lambda is rejected
//! rule: so a *borrowed* class reference is not a valid operand
//! cites: 31.1
//! expect: reject
//! diagnostic: must be an OWNED exception
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R();
      if true then
        var b := c;
        var g := lambda(): int => do throw b; end;
        g();
      end;
      return 0;
    end;
    try f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
