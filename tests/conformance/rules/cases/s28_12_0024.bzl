//! case: s28_12_0024
//! doc: spec
//! section: 28.12
//! title: await using at an Object static type over a class with no IAsyncDisposable disposes synchronously
//! rule: Only an object whose class does not implement `IAsyncDisposable` takes the
//!   synchronous disposal above.
//! cites: 28.12
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
    await using var u: Object := new Plain();
    Console.WriteLine("body");
  end Main;
end Program;
