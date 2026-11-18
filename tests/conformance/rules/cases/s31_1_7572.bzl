//! case: s31_1_7572
//! doc: spec
//! section: 31.1
//! title: two throw sites of two distinct enclosing-lambda borrows in a nested lambda are each rejected
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
      var c := new R(); var b := c; var d := c;
      var g := lambda(): int => do
        if true then throw b; end;
        throw d;
      end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
