//! case: s28_3_7_7612
//! doc: spec
//! section: 28.3.7
//! title: a lambda may throw an owned local it declares itself
//! rule: reads borrow (the producers rows above), and assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of it, and every other owned-consuming use of it are compile errors
//! cites: 28.3, 31.1
//! expect: run
//! stdout: caught
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var g := lambda(): int => do var c := new R(); throw c; end;
      _ := g();
      return 0;
    end;
    try _ := f(); catch var x: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
