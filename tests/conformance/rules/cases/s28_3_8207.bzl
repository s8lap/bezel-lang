//! case: s28_3_8207
//! doc: spec
//! section: 28.3
//! title: a closure throwing fresh conditional arms is admitted when it also owns a local of its
//!   own
//! rule: reads borrow (the producers rows above)
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
//! notes: Admitting twin of s28_3_8201 (#8958): the closure's own owned local `d` is declared
//!   inside the closure, so it is not a captured owned binding; the thrown conditional is
//!   fresh on both arms.
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var n := 1;
    var f := lambda(): int => do
      var d := new R();
      throw (if n > 0 then new R() else new R());
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
