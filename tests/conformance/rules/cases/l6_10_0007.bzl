//! case: l6_10_0007
//! doc: stdlib
//! section: 6.10
//! title: Spawn admits a capture-bearing body bounded by the scope
//! rule: method Spawn(scoped body: () -> Task); // start a child (§28.14
//!   storage-bounded)
//! cites: 6.10, 28.14
//! expect: run
//! stdout: 3
module Conf;

import System.Threading;

public sealed class Counter
  public Total: int;
  public constructor() Total := 0; end constructor;
end Counter;

public class Program
  public static async method Main(): Task
    using var counter := new Counter();
    await using var scope := new TaskScope();
    scope.Spawn(async lambda() => do
      await Task.Delay(0);
      counter.Total := counter.Total + 1;
    end);
    scope.Spawn(async lambda() => do
      await Task.Delay(0);
      counter.Total := counter.Total + 1;
    end);
    scope.Spawn(async lambda() => do
      await Task.Delay(0);
      counter.Total := counter.Total + 1;
    end);
    await scope.Join();
    Console.WriteLine(counter.Total);
  end Main;
end Program;
