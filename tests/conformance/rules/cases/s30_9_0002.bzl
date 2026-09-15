//! case: s30_9_0002
//! doc: spec
//! section: 30.9
//! title: an exception thrown in the synchronous prefix faults the returned task
//! rule: An exception thrown in the synchronous prefix of an async method
//!   (before any `await`) faults the returned task, not the immediate
//!   caller.
//! cites: 30.9
//! expect: run
//! stdout: early
module Conf;

public class Worker
  public constructor()
  end constructor;

  public async method FailFastAsync(): Task
    throw new InvalidOperationException("early");
  end FailFastAsync;
end Worker;

public class Program
  public static async method Main(): Task
    try
      await new Worker().FailFastAsync();
    catch
      var ex: InvalidOperationException => Console.WriteLine(ex.Message)
    end try;
  end Main;
end Program;
