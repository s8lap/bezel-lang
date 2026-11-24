//! case: s28_3_8216
//! doc: spec
//! section: 28.3
//! title: a closure throwing fresh case arms is admitted when only an arm guard reads a captured
//!   owned local
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 19.3
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8211 (#9043): the guard reads `c` through a call, a borrow read,
//!   and the arm values are fresh.
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
      throw (case n of 1 when c.Flag() => new R(), _ => new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
