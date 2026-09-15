//! case: l6_6_0003
//! doc: stdlib
//! section: 6.6
//! title: SetException's owned exception is re-raised to the awaiter
//! rule: Fault-path ownership (v0.179). SetException/TrySetException (all
//!   forms) take the exception owned: the produced task becomes the single
//!   owner of its fault exception (awaiters borrow it on re-raise).
//! cites: 6.6, 30.9
//! expect: run
//! stdout: boom
module Conf;

import System.Threading;

public class Program
  public static async method FailAsync(): Task<int>
    using var tcs := new TaskCompletionSource<int>();
    tcs.SetException(new InvalidOperationException("boom"));
    return await tcs.Task;
  end FailAsync;

  public static async method Main(): Task
    try
      var n := await FailAsync();
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
