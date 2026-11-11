//! case: s7_5_4602
//! doc: spec
//! section: 7.5
//! title: a ?. read of a TaskCompletionSource's Task through a non-null receiver yields the task
//! rule: `x?.M` returns null if `x` is null; otherwise it performs the access.
//! cites: 30.11
//! expect: run
//! stdout: 42
//! issue: 7423
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    using var tcs: TaskCompletionSource<int>? := new TaskCompletionSource<int>();
    var t := tcs?.Task;
    _ := tcs?.TrySetResult(41);
    var v := await t!;
    Console.WriteLine(v + 1);
  end Main;
end Program;
