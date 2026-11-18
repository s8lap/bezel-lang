//! case: s31_1_7575
//! doc: spec
//! section: 31.1
//! title: a nested lambda throws its own fresh owned local and the caller catches it
//! rule: `throw expr` requires `expr` to be an owned instance of an `Exception`-derived class type
//! cites: 31.1
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
      var c := new R(); var b := c;
      var g := lambda(): int => do var e := new R(); throw e; end;
      try _ := g(); catch var x: R => Console.WriteLine("caught"); end;
      return 0;
    end;
    _ := f();
    return 0;
  end Main;
end Program;
