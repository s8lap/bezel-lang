//! case: s28_12_0005
//! doc: spec
//! section: 28.12
//! title: a direct DisposeAsync call awaited outside await using is legal and reclamation follows
//! rule: A direct DisposeAsync() call awaited outside await using is legal;
//!   the awaited task's completion is logical-disposal completion, and
//!   reclamation follows exactly as if await using had issued it.
//! cites: 28.12
//! expect: run
//! output: async cleanup
//!   inner disposed
//!   done
module Conf;

import System.Threading;

public class Inner
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("inner disposed");
  end destructor;
end Inner;

public class Resource: IAsyncDisposable
  private owned _inner: Inner;

  public constructor()
    _inner := new Inner();
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
    using var r := new Resource();
    await r.DisposeAsync();
    Console.WriteLine("done");
  end Main;
end Program;
