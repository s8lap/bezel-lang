//! case: s28_3_8208
//! doc: spec
//! section: 28.3
//! title: a closure throwing fresh conditional arms is admitted after the captured owned local was
//!   moved out
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 28.8.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8204 (#8958): `c` is transferred to `d` before the closure is
//!   formed, and the closure's body no longer names a captured owned binding.
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
    var d := move c;
    var f := lambda(): int => do
      throw (if n > 0 then new R() else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
