//! case: l6_12_0004
//! doc: stdlib
//! section: 6.12
//! title: a capture-free lambda is an admitted Parallel spawn position
//! rule: The body/selector/combiner parameters are spawn positions:
//!   capture-free (a non-capturing lambda or a static method reference).
//! cites: 6.12, 30.14
//! expect: run
//! stdout: 2
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    var items: Array<int> := [1, 2, 3];
    var results := await Parallel.Map(items, lambda(n) => n * 2);
    Console.WriteLine(results[0]);
  end Main;
end Program;
