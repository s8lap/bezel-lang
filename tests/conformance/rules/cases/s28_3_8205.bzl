//! case: s28_3_8205
//! doc: spec
//! section: 28.3
//! title: a closure throwing fresh conditional arms is admitted when only the condition reads a
//!   captured owned
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8201 (#8958): `c` is read only in the condition, a borrow read,
//!   and each arm is a fresh owned value, so the thrown operand is owned.
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
      throw (if c.Flag() then new R() else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
