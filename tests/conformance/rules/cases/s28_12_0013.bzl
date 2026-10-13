//! case: s28_12_0013
//! doc: spec
//! section: 28.12
//! title: a second DisposeAsync during the first's window returns a completed task at once
//! rule: A second call to `Dispose()` or `DisposeAsync()` after either has
//!   **begun** is a no-op (§28.2's idempotence rule): the per-instance flag
//!   records the beginning of logical disposal — at the `Dispose()` call, at
//!   the direct `DisposeAsync()` call, or at `await using`'s scope exit,
//!   before the body's first statement — and the `DisposeAsync()` no-op
//!   returns a completed task.
//! cites: 28.12, 28.2
//! expect: run
//! warns: yes
//! output: second returned
//!   async cleanup
//!   done
module Conf;

public class Resource: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(50);
    Console.WriteLine("async cleanup");
  end DisposeAsync;

  destructor
    Console.WriteLine("sync cleanup");
  end destructor;
end Resource;

public class Program
  public static async method Main(): Task
    using var r := new Resource();
    using var first := r.DisposeAsync();
    await r.DisposeAsync();
    Console.WriteLine("second returned");
    await first;
    Console.WriteLine("done");
  end Main;
end Program;
