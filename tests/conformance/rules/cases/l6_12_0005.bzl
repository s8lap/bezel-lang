//! case: l6_12_0005
//! doc: stdlib
//! section: 6.12
//! title: ForEach invokes its body exactly once per element
//! rule: public static method ForEach<A>(items: Array<A>, body: (A) -> (),
//!   degree: int := 0): owned Task where A: sendable, notnull;
//! cites: 6.12, 30.14
//! expect: run
//! output: 0
//!   5
module Conf;

import System.Threading;

public class Program
  private static readonly _count: AtomicInteger<int> := new AtomicInteger<int>(0);

  public static async method Main(): Task
    var items: Array<int> := [1, 2, 3, 4, 5];
    Console.WriteLine(_count.Value);
    await Parallel.ForEach(items, lambda(n) => do _count.Increment(); end);
    Console.WriteLine(_count.Value);
  end Main;
end Program;
