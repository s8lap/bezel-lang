//! case: s33_3_0007
//! doc: spec
//! section: 33.3
//! title: a spawned domain still running when Main completes is abandoned, its owned state undisposed
//! rule: **Live domains at exit:** when `Main` completes, spawned domains that have
//!   not completed are **abandoned**: the runtime signals no cancellation, runs no
//!   further continuations of theirs, and proceeds to the shutdown sequence — their
//!   owned state is reclaimed by the process exit, not by disposal, and their
//!   pending effects are lost.
//! cites: 33.3, 30.14
//! expect: run
//! output: main
//! notes: the worker's delay is ten minutes, so a runtime that waited for it times
//!   out; one that disposed its `using var` at exit would print "closed w".
module Conf;

import System.Threading;

public class Res
  public property Name: string;

  public constructor(name: string)
    Name := name;
  end constructor;

  destructor
    Console.WriteLine($"closed {Name}");
  end destructor;
end Res;

public class Program
  public static async method Work(): Task
    using var r := new Res("w");
    await Task.Delay(600000);
    Console.WriteLine($"late {r.Name}");
  end Work;

  public static method Main()
    Task.FireAndForget(Domain.Spawn(Program.Work));
    Console.WriteLine("main");
  end Main;
end Program;
