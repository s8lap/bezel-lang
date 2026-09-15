//! case: s22_7_0007
//! doc: spec
//! section: 22.7
//! title: a caught exception inside a static constructor does not fail the type
//! rule: An exception caught inside the static constructor's body does not
//!   transition the type to the failed state — the constructor completes
//!   normally and static initialization is considered successful.
//! cites: 22.7
//! expect: run
//! output: caught
//!   True
module Conf;

public class Config
  public static readonly Ready: bool;

  static constructor()
    try
      throw new InvalidOperationException("boom");
    catch
      var ex: InvalidOperationException => Console.WriteLine("caught")
    end try;
    Ready := true;
  end constructor;
end Config;

public class Program
  public static method Main()
    Console.WriteLine(Config.Ready);
  end Main;
end Program;
