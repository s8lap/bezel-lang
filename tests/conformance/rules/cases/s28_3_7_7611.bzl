//! case: s28_3_7_7611
//! doc: spec
//! section: 28.3.7
//! title: throwing an enclosing lambda's owned local from a nested lambda is rejected
//! rule: reads borrow (the producers rows above), and assignment to it, a marked transfer from it, an explicit `Dispose()` or discard of it, and every other owned-consuming use of it are compile errors
//! cites: 28.3, 31.1
//! expect: reject
//! diagnostic: cannot throw captured owned binding
module Conf;
import System;
public class R: Exception
  public constructor() end constructor;
end R;
public class Program
  public static method Main(): int
    var f := lambda(): int => do
      var c := new R();
      var g := lambda(): int => do throw c; end;
      _ := g();
      return 0;
    end;
    try _ := f(); catch var x: R => Console.Write("caught"); end;
    return 0;
  end Main;
end Program;
