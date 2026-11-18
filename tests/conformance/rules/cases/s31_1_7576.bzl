//! case: s31_1_7576
//! doc: spec
//! section: 31.1
//! title: a nested lambda re-raises the caught exception with the bare throw and the outer caller catches it
//! rule: To re-raise the exception being handled, use the bare re-raise
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
      var g := lambda(): int => do
        try
          throw new R();
        catch var x: R =>
          throw;
        end;
      end;
      _ := g();
      return 0;
    end;
    try _ := f(); catch var y: R => Console.WriteLine("caught"); end;
    return 0;
  end Main;
end Program;
