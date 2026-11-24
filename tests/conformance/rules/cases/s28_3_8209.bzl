//! case: s28_3_8209
//! doc: spec
//! section: 28.3
//! title: a conditional over a captured owned local as a call receiver in a closure is admitted
//! rule: reads borrow (the producers rows above)
//! cites: 28.3
//! expect: run
//! stdout: 1
//! notes: Over-rejection guard for #8958: the fix reports a captured owned binding only as a throw
//!   operand arm. The same conditional as a receiver in an `if` condition is a borrow read;
//!   it returns 1 because Flag() is true.
module Conf;
import System;
public class R
  public constructor() end constructor;
  public method Flag(): bool return true; end Flag;
end R;
public class Program
  public static method Main(): int
    var c := new R(); var n := 1;
    var f := lambda(): int => do
      if (if n > 0 then c else c).Flag() then return 1; end;
      return 0;
    end;
    Console.WriteLine(f());
    return 0;
  end Main;
end Program;
