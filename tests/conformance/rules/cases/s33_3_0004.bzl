//! case: s33_3_0004
//! doc: spec
//! section: 33.3
//! title: Environment.Exit does not dispose an owned parameter of a live frame
//! rule: **The calling thread's stack is not unwound:** no `finally` block of any
//!   live frame runs, no `using var`, owned local, `owned` parameter or unbound
//!   owned temporary of any live frame is disposed
//! cites: 33.3, 28.3
//! expect: run
//! output: consuming p
//! notes: without the Exit call, `Consume` would dispose `r` at its exit and print
//!   "closed p".
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
  public static method Consume(owned r: Res)
    Console.WriteLine($"consuming {r.Name}");
    Environment.Exit(0);
  end Consume;

  public static method Main()
    Consume(new Res("p"));
  end Main;
end Program;
