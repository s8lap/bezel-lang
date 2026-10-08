//! case: s28_12_0010
//! doc: spec
//! section: 28.12
//! title: await using runs the destructor of a level whose DisposeAsync is abstract
//! rule: At each level of the reclamation chain the destructor body is
//!   skipped if that level declares or overrides a non-`abstract`
//!   `DisposeAsync`, and runs if that level does not — a base class whose
//!   only cleanup is its destructor, and a level whose `DisposeAsync` is
//!   `abstract` (it performs no cleanup, and no override can chain to it),
//!   are cleaned up exactly as on the synchronous path.
//! cites: 28.12, 28.4, 12.5
//! expect: run
//! output: working
//!   der async cleanup
//!   base destructor
module Conf;

public abstract class Base: IAsyncDisposable
  public constructor()
  end constructor;

  public abstract method DisposeAsync(): Task;

  destructor
    Console.WriteLine("base destructor");
  end destructor;
end Base;

public class Der: Base
  public constructor()
  end constructor;

  public async override method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("der async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("der destructor");
  end destructor;
end Der;

public class Program
  public static async method Main(): Task
    await using var d := new Der();
    Console.WriteLine("working");
  end Main;
end Program;
