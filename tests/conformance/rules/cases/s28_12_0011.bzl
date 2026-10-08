//! case: s28_12_0011
//! doc: spec
//! section: 28.12
//! title: plain using over an abstract-DisposeAsync hierarchy runs every level's destructor
//! rule: For plain `using` (or a synchronous `Dispose()` call) on a class that
//!   implements both, the destructor body runs synchronously and
//!   `DisposeAsync` is **not** invoked — the class's two cleanup paths are
//!   alternatives, not a sequence.
//! cites: 28.12, 28.4, 12.5
//! expect: run
//! output: working
//!   der destructor
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
  public static method Main()
    using var d := new Der();
    Console.WriteLine("working");
  end Main;
end Program;
