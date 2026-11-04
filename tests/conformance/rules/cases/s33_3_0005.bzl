//! case: s33_3_0005
//! doc: spec
//! section: 33.3
//! title: Environment.Exit does not dispose an unbound owned temporary of a live frame
//! rule: **The calling thread's stack is not unwound:** no `finally` block of any
//!   live frame runs, no `using var`, owned local, `owned` parameter or unbound
//!   owned temporary of any live frame is disposed
//! cites: 33.3, 28.3
//! expect: run
//! output: using t
//! notes: Main's `new Res("t")` is an unbound owned temporary lent to `Use`; it would
//!   be disposed after the call's full expression, which Exit never lets complete.
module Conf;

public class Res
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"closed {Name}");
  end destructor;
end Res;

public class Program
  public static method Use(r: Res)
    Console.WriteLine($"using {r.Name}");
    Environment.Exit(0);
  end Use;

  public static method Main()
    Use(new Res("t"));
  end Main;
end Program;
