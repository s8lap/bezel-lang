//! case: l6_12_0003
//! doc: stdlib
//! section: 6.12
//! title: a capturing lambda body is rejected as a Parallel spawn position
//! rule: The body/selector/combiner parameters are spawn positions:
//!   capture-free (a non-capturing lambda or a static method reference).
//! cites: 6.12, 30.14
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    var factor := 2;
    var items: Array<int> := [1, 2, 3];
    var results := await Parallel.Map(items, lambda(n) => n * factor);
    Console.WriteLine(results[0]);
  end Main;
end Program;
