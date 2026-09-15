//! case: s28_12_0006
//! doc: spec
//! section: 28.12
//! title: a second call to DisposeAsync after the first has run is a no-op
//! rule: A second call to Dispose() or DisposeAsync() after either has run
//!   is a no-op — the runtime tracks completion at the granularity of the
//!   object, not per-method.
//! cites: 28.12, 28.2
//! expect: run
//! output: async cleanup
//!   done
module Conf;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("async cleanup");
  end DisposeAsync;
end Resource;

public class Program
  public static async method Main(): Task
    var r := new Resource();
    await r.DisposeAsync();
    await r.DisposeAsync();
    Console.WriteLine("done");
  end Main;
end Program;
