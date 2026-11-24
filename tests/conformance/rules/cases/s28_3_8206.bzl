//! case: s28_3_8206
//! doc: spec
//! section: 28.3
//! title: a closure throwing a conditional whose arm is a call on a captured owned local is
//!   admitted
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8202 (#8958): the arm is `c.Make()`, a call returning `owned R`,
//!   a fresh owned value; `c` is only the borrowed receiver. Nothing consumes the captured
//!   binding.
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
      throw (if n > 0 then c.Make() else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
