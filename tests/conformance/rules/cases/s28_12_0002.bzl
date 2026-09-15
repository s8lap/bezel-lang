//! case: s28_12_0002
//! doc: spec
//! section: 28.12
//! title: plain using runs the destructor synchronously and never invokes DisposeAsync
//! rule: For plain using (or a synchronous Dispose() call) on a class that
//!   implements both, the destructor body runs synchronously and
//!   DisposeAsync is not invoked — the class's two cleanup paths are
//!   alternatives, not a sequence.
//! cites: 28.12
//! expect: run
//! output: working
//!   sync cleanup
module Conf;

import System.Threading;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("sync cleanup");
  end destructor;
end Resource;

public class Program
  public static method Main()
    using var r := new Resource();
    Console.WriteLine("working");
  end Main;
end Program;
