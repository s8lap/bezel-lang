//! case: l6_6_0006
//! doc: stdlib
//! section: 6.6
//! title: a fresh cause-chained exception is the admitted spelling from a catch arm
//! rule: A caller that holds only a caught borrow (spec §31.1) constructs a
//!   fresh cause-chained exception instead: tcs.SetException(new
//!   Exception(ex.Message, ex));
//! cites: 6.6, 31.1, 31.4
//! expect: run
//! stdout: inner
module Conf;

import System.Threading;

public class Program
  public static async method Attempt(): Task
    using var tcs := new TaskCompletionSource();
    try
      throw new InvalidOperationException("inner");
    catch
      var ex: InvalidOperationException => tcs.SetException(new Exception(ex.Message, ex)),
    end try;
    await tcs.Task;
  end Attempt;

  public static async method Main(): Task
    try
      await Attempt();
    catch
      var ex: Exception => Console.WriteLine(ex.Message),
    end try;
  end Main;
end Program;
