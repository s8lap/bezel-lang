//! case: s30_4_0003
//! doc: spec
//! section: 30.4
//! title: a non-generic Task may be awaited from multiple places
//! rule: A `Task` or `Task<T>` may be awaited from multiple places,
//!   sequentially or concurrently. Every awaiter sees the same eventual
//!   state. The underlying computation runs once.
//! cites: 30.4
//! expect: run
//! stdout: twice
module Conf;

public class Counter
  public static async method BumpAsync(): Task
    await Task.Delay(0);
  end BumpAsync;
end Counter;

public class Program
  public static async method Main(): Task
    using var t := Counter.BumpAsync();
    await t;
    await t;
    Console.WriteLine("twice");
  end Main;
end Program;
