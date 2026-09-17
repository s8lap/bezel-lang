//! case: l6_12_0002
//! doc: stdlib
//! section: 6.12
//! title: Reduce over an associative combiner with an identity seed matches the serial fold
//! rule: The serial-fold correspondence holds exactly when the combiner is
//!   associative and the seed is its identity.
//! cites: 6.12, 30.14
//! expect: run
//! stdout: 10
module Conf;

import System.Threading;

public class Program
  public static method Identity(n: int): int => n;
  public static method Add(a: int, b: int): int => a + b;

  public static async method Main(): Task
    var items: Array<int> := [1, 2, 3, 4];
    var total := await Parallel.Reduce(items, 0, Program.Identity, Program.Add);
    Console.WriteLine(total);
  end Main;
end Program;
