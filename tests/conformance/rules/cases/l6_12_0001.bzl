//! case: l6_12_0001
//! doc: stdlib
//! section: 6.12
//! title: Map reassembles results in input order
//! rule: Results in input order; first fault wins, surfaced at the await
//!   as DomainFaultedException.
//! cites: 6.12, 30.14
//! expect: run
//! output: 1
//!   4
//!   9
//!   16
module Conf;

import System.Threading;

public class Program
  public static method Square(n: int): int => n * n;

  public static async method Main(): Task
    var items: Array<int> := [1, 2, 3, 4];
    var results := await Parallel.Map(items, Program.Square);
    Console.WriteLine(results[0]);
    Console.WriteLine(results[1]);
    Console.WriteLine(results[2]);
    Console.WriteLine(results[3]);
  end Main;
end Program;
