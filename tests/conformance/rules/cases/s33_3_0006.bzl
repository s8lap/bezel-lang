//! case: s33_3_0006
//! doc: spec
//! section: 33.3
//! title: Environment.Exit from a spawned domain ends the program before Main's continuation resumes
//! rule: **`Environment.Exit(code)`:** callable from any domain … it initiates the
//!   same shutdown sequence (no further continuations anywhere, static `owned`
//!   disposal on the dedicated shutdown thread, then process exit with `code`).
//! cites: 33.3, 30.14
//! expect: run
//! output: worker
//! notes: Main's continuation after the await would print "after"; the shutdown
//!   sequence runs no further continuations anywhere.
module Conf;

import System.Threading;

public class Program
  public static method Work()
    Console.WriteLine("worker");
    Environment.Exit(0);
  end Work;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Work);
    await t;
    Console.WriteLine("after");
  end Main;
end Program;
