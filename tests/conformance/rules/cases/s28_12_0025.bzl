//! case: s28_12_0025
//! doc: spec
//! section: 28.12
//! title: await using at a base static type over a derived class with no IAsyncDisposable runs the chain
//! rule: Over an owned right-hand side with no `DisposeAsync` to await — a
//!   value-type, runtime-managed or move-only right-hand side, or an object
//!   whose class does not implement `IAsyncDisposable` — scope exit takes the
//!   synchronous disposal (or the no-op) and likewise awaits nothing.
//! cites: 28.12, 28.4
//! expect: run
//! output: body
//!   DER-SYNC
//!   BASE-SYNC

module Conf;

public class Base
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("BASE-SYNC");
  end destructor;
end Base;

public class PlainDer: Base
  public constructor()
  end constructor;

  destructor
    Console.WriteLine("DER-SYNC");
  end destructor;
end PlainDer;

public class Program
  public static async method Main(): Task
    await using var b: Base := new PlainDer();
    Console.WriteLine("body");
  end Main;
end Program;
