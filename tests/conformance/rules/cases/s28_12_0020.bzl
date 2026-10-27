//! case: s28_12_0020
//! doc: spec
//! section: 28.12
//! title: await using at an IAsyncDisposable static type awaits the run-time object's DisposeAsync
//! rule: Where that class implements `IAsyncDisposable`, scope exit awaits its
//!   `DisposeAsync` and reclamation follows as step 2 states: through a
//!   base-class static type that does not implement `IAsyncDisposable` (`await
//!   using var b: Base := new Der();` with `class Der: Base, IAsyncDisposable`
//!   awaits `Der.DisposeAsync`, skips `Der`'s destructor and runs `Base`'s —
//!   the hierarchy step 2 already traces), through `Object`, and through any
//!   interface — `IAsyncDisposable` or one deriving from it, whose
//!   `DisposeAsync` dispatches as any interface member does, or another such as
//!   `IDisposable`, which every class implements.
//! cites: 28.12
//! expect: run
//! issue: 7328
//! output: body
//!   ASYNC

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
  public static async method Main(): Task
    await using var d: IAsyncDisposable := new Solo();
    Console.WriteLine("body");
  end Main;
end Program;
