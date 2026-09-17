//! case: s28_12_0004
//! doc: spec
//! section: 28.12
//! title: implicit discharge always takes the synchronous Dispose path, even inside an async body
//! rule: Implicit discharge is synchronous — the implicit disposal paths
//!   always take the synchronous Dispose() path, on every edge and in
//!   every context including async bodies; an implicit disposal point
//!   never awaits.
//! cites: 28.12
//! expect: run
//! output: sync cleanup
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
    do
      var r := new Resource();
    end;
    Console.WriteLine("done");
  end Main;
end Program;
