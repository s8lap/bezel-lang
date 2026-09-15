//! case: l6_6_0005
//! doc: stdlib
//! section: 6.6
//! title: a caught exception borrow cannot feed SetException's owned parameter
//! rule: A caller that holds only a caught borrow (spec §31.1) constructs a
//!   fresh cause-chained exception instead: tcs.SetException(new
//!   Exception(ex.Message, ex));
//! cites: 6.6, 31.1
//! expect: reject
//! diagnostic: owned
module Conf;

import System.Threading;

public class Program
  public static async method Attempt(): Task
    using var tcs := new TaskCompletionSource();
    try
      throw new InvalidOperationException("inner");
    catch
      var ex: InvalidOperationException => tcs.SetException(ex),
    end try;
    await tcs.Task;
  end Attempt;
end Program;
