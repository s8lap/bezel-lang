//! case: s28_12_0019
//! doc: spec
//! section: 28.12
//! title: await using over a class that does not implement IAsyncDisposable takes the synchronous disposal
//! rule: Over an owned right-hand side with no `DisposeAsync` to await — a
//!   value-type, runtime-managed or move-only right-hand side, or an object
//!   whose class does not implement `IAsyncDisposable` — scope exit takes the
//!   synchronous disposal (or the no-op) and likewise awaits nothing.
//! cites: 28.12, 28.4
//! expect: run
//! output: body
//!   PLAIN-SYNC

module Conf;

public class Plain
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("PLAIN-SYNC");
  end destructor;
end Plain;

public class Program
  public static async method Main(): Task
    await using var u := new Plain();
    Console.WriteLine("body");
  end Main;
end Program;
