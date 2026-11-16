//! case: s30_11_0016
//! doc: spec
//! section: 30.11
//! title: awaiting tcs.Task inside the source's scope and returning the value is well-formed
//! rule: so `return await tcs.Task;` inside the scope (every example below) is well-formed
//! cites: 30.11, 28.3
//! expect: run
//! output: 1
//! issue: 8616
module Conf;

import System.Threading;

public class Program
  public static async method Get(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetResult(1);
    return await tcs.Task;
  end Get;

  public static async method Main(): Task
    var n := await Get();
    Console.WriteLine(n);
  end Main;
end Program;
