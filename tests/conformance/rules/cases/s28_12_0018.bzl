//! case: s28_12_0018
//! doc: spec
//! section: 28.12
//! title: await using over a function-typed right-hand side runs and disposes nothing
//! rule: Over an owned right-hand side with no `DisposeAsync` to await — a
//!   value-type, runtime-managed or move-only right-hand side, or an object
//!   whose class does not implement `IAsyncDisposable` — scope exit takes the
//!   synchronous disposal (or the no-op) and likewise awaits nothing.
//! cites: 28.12, 28.9
//! expect: run
//! output: 1

module Conf;

public class Program
  public static async method Main(): Task
    await using var f := lambda() => 1;
    Console.WriteLine(f());
  end Main;
end Program;
