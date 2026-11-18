//! case: s31_1_7571
//! doc: spec
//! section: 31.1
//! title: a coalesce-spelled throw of the enclosing lambda's borrowed local in a nested lambda is rejected
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
      var c := new R(); var b := c; var n: int? := null;
      var g := lambda(): int => do return n ?? throw b; end;
      return g();
    end;
    try f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
