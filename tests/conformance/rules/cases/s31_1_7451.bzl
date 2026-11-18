//! case: s31_1_7451
//! doc: spec
//! section: 31.1
//! title: a nested lambda throwing a borrow of its own owned local is rejected
//! rule: The operand is consumed as an owned value under the ownership rules below, so a *borrowed*
//!   class reference is not a valid operand
//! cites: 31.1, 28.3
//! expect: reject
//! diagnostic: the thrown value must be an OWNED exception
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var g := lambda(): int => do var c := new R(); var b := c; throw b; end;
      g();
      return 0;
    end;
    try f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
