//! case: s28_3_8215
//! doc: spec
//! section: 28.3
//! title: a closure throwing fresh case arms is admitted when only the scrutinee reads a captured
//!   owned local
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8211 (#9043): `c` is read only as the scrutinee's receiver, a
//!   borrow read; every arm is a fresh owned value.
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
      throw (case c.Code() of 1 => new R(), _ => new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
