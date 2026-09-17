//! case: s30_9_0001
//! doc: spec
//! section: 30.9
//! title: an exception thrown after an await faults the task and is re-raised by the caller's await
//! rule: An exception thrown after an `await` faults the task. Awaiting a
//!   faulted task re-raises the exception with the original stack trace
//!   preserved.
//! cites: 30.9
//! expect: run
//! stdout: boom
module Conf;

public class Worker
  public static async method FailAsync(): Task
    await Task.Delay(0);
    throw new InvalidOperationException("boom");
  end FailAsync;
end Worker;

public class Program
  public static async method Main(): Task
    try
      await Worker.FailAsync();
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
