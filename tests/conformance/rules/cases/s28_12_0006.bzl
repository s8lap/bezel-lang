//! case: s28_12_0006
//! doc: spec
//! section: 28.12
//! title: a second call to DisposeAsync after the first has begun is a no-op
//! rule: A second call to `Dispose()` or `DisposeAsync()` after either has
//!   **begun** is a no-op (§28.2's idempotence rule): the per-instance flag
//!   records the beginning of logical disposal.
//! cites: 28.12, 28.2
//! expect: run
//! warns: yes
//! output: async cleanup
//!   done
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
    using var r := new Resource();
    await r.DisposeAsync();
    await r.DisposeAsync();
    Console.WriteLine("done");
  end Main;
end Program;
