//! case: s28_12_0028
//! doc: spec
//! section: 28.12
//! title: await using declared Object over a boxed value type disposes nothing
//! rule: Over an owned right-hand side with no `DisposeAsync` to await — a
//!   value-type, runtime-managed or move-only right-hand side, or an object
//!   whose class does not implement `IAsyncDisposable` — scope exit takes the
//!   synchronous disposal (or the no-op) and likewise awaits nothing.
//! cites: 28.12, 28.9, 8.6
//! expect: run
//! output: body

module Conf;

public class Program
  public static async method Main(): Task
    await using var x: Object := 5;
    Console.WriteLine("body");
  end Main;
end Program;
