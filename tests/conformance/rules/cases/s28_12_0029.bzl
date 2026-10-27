//! case: s28_12_0029
//! doc: spec
//! section: 28.12
//! title: plain using at an IDisposable static type runs the run-time object's destructor
//! rule: For plain `using` (or a synchronous `Dispose()` call) on a class that
//!   implements both, the destructor body runs synchronously and
//!   `DisposeAsync` is **not** invoked — the class's two cleanup paths are
//!   alternatives, not a sequence.
//! cites: 28.12
//! expect: run
//! output: body
//!   SYNC

module Conf;

import System.Threading;

public class Solo: IAsyncDisposable
  public constructor()
  end constructor;

  public async method DisposeAsync(): Task
    await Task.Delay(0);
    Console.WriteLine("ASYNC");
  end DisposeAsync;

  destructor
    Console.WriteLine("SYNC");
  end destructor;
end Solo;

public class Program
  public static method Main()
    using var d: IDisposable := new Solo();
    Console.WriteLine("body");
  end Main;
end Program;
