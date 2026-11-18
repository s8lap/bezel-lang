//! case: s31_1_7450
//! doc: spec
//! section: 31.1
//! title: a lambda throws its own owned local as the right operand of ??
//! rule: The operand is consumed as an owned value under the ownership rules below, so a *borrowed*
//!   class reference is not a valid operand
//! cites: 31.1, 28.3
//! expect: run
//! output: caught
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;

public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R(); var n: int? := null;
      return n ?? throw c;
    end;
    try f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
