//! case: l6_8_0001
//! doc: stdlib
//! section: 6.8
//! title: Spawn's value-typed result crosses the join as a plain Task<R>
//! rule: Spawn<R>(entry: () -> R): owned Task<R> where R: sendable, notnull;
//!   // Value-typed or string result (Pattern A).
//! cites: 6.8, 30.14
//! expect: run
//! stdout: 42
module Conf;

import System.Threading;

public class Program
  public static method Compute(): int => 6 * 7;

  public static async method Main(): Task
    using var t := Domain.Spawn(Program.Compute);
    var n := await t;
    Console.WriteLine(n);
  end Main;
end Program;
