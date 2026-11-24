//! case: s28_3_8217
//! doc: spec
//! section: 28.3
//! title: a closure throwing a case expression whose arm is a call on a captured owned local is
//!   admitted
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8212 (#9043): the arm `c.Make()` returns `owned R`, a fresh
//!   owned value.
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
      throw (case n of 1 => c.Make(), _ => new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
