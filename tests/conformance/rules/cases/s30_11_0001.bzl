//! case: s30_11_0001
//! doc: spec
//! section: 30.11
//! title: TaskCompletionSource<T> completes a Task<T> imperatively for non-class T
//! rule: A `TaskCompletionSource<T>` produces a task whose state is
//!   controlled imperatively. For non-class T ... the produced `Task<T>`
//!   carries no ownership across the await boundary.
//! cites: 30.11
//! expect: run
//! stdout: 7
module Conf;

import System.Threading;

public class Program
  public static async method ReadAsync(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.TrySetResult(7);
    return await tcs.Task;
  end ReadAsync;

  public static async method Main(): Task
    var n := await ReadAsync();
    Console.WriteLine(n);
  end Main;
end Program;
