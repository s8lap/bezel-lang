//! case: l2_21_0003
//! doc: stdlib
//! section: 2.21
//! title: a ScopeGuard's action runs on the throwing edge
//! rule: On any throwing edge the guard's disposal runs the undo — a using
//!   var's dispose runs on the exception edge exactly as on the normal one.
//! cites: 2.21, 28.10
//! expect: run
//! stdout: rolled back
module Conf;

public class Boom: Exception
  public constructor() base();
  end constructor;
end Boom;

public class Program
  public static method Main()
    try
      using var g := new ScopeGuard(lambda() => Console.WriteLine("rolled back"));
      throw new Boom();
    catch
      Boom => Console.WriteLine("caught")
    end;
  end Main;
end Program;
