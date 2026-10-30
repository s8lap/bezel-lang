//! case: s26_5_0049
//! doc: spec
//! section: 26.5
//! title: the most specific extension head wins over a base-class head for Task<T> and OwningTask<T>
//!   receivers
//! rule: An extension on E1 is **more specific** than one on E2 when E1 is a subtype of E2 and E2 is
//!   not a subtype of E1.
//! cites: 26.5, 30.3
//! expect: run
//! output: taskTtask
module Conf;

public class K
  public constructor() end constructor;
end K;

extension Object
  public method W(): string => "obj";
end extension;

extension Task
  public method W(): string => "task";
end extension;

extension Task<T>
  public method W(): string => "taskT";
end extension;

public class Program
  public static async method P(): OwningTask<K>
    return new K();
  end P;

  public static method Main(): int
    using var a := Task.FromResult(1);
    using var c := P();
    Console.WriteLine(a.W() + c.W());
    return 0;
  end Main;
end Program;
