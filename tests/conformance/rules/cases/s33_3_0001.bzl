//! case: s33_3_0001
//! doc: spec
//! section: 33.3
//! title: Environment.Exit unwinds nothing - no finally runs and no using var is disposed
//! rule: **The calling thread's stack is not unwound:** no `finally` block of
//!   any live frame runs, no `using var`, owned local, `owned` parameter or
//!   unbound owned temporary of any live frame is disposed, and no suspended
//!   frame of any domain resumes — their owned state is reclaimed by the
//!   process exit, not by disposal
//! cites: 33.3, 28.10, 28.3
//! expect: run
//! output: exiting
module Conf;

public class Res
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("closed");
  end destructor;
end Res;

public class Demo
  public static method Run()
    using var r := new Res();
    try
      Console.WriteLine("exiting");
      Environment.Exit(0);
    finally
      Console.WriteLine("finally");
    end try;
  end Run;

  public static method Main()
    Run();
    Console.WriteLine("returned");
  end Main;
end Demo;
