//! case: s28_12_0001
//! doc: spec
//! section: 28.12
//! title: await using invokes DisposeAsync and the destructor body is not also run
//! rule: For await using, the awaited DisposeAsync() performs whatever
//!   asynchronous cleanup the class defines; the destructor body (if any)
//!   is not also run, because DisposeAsync supersedes it.
//! cites: 28.12
//! expect: run
//! output: working
//!   async cleanup
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
  public static async method Main(): Task
    await using var r := new Resource();
    Console.WriteLine("working");
  end Main;
end Program;
