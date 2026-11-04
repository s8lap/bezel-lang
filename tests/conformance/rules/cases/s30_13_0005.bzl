//! case: s30_13_0005
//! doc: spec
//! section: 30.13
//! title: an async Main runs as a domain continuation, where a helper's blocking wait raises
//! rule: An `async` `Main` is driven as the main domain's first continuation,
//!   with the startup thread blocked on its task's completion (§33.2). …
//!   Where static rejection cannot see the call (a non-`async` helper invoked
//!   from a continuation), the runtime raises `InvalidOperationException` at
//!   the call site instead of parking
//! cites: 30.13, 30.3, 33.2
//! expect: run
//! stdout: blocked
//! notes: the second sentence is §30.3's runtime guard. Were async Main on the
//!   startup thread, the Result read would block forever on a task nothing
//!   completes.
module Conf;

import System.Threading;

public class Program
  public static method Block(t: Task<int>): int
    return t.Result;
  end Block;

  public static async method Main(): Task
    using var tcs := new TaskCompletionSource<int>();
    try
      Console.WriteLine(Block(tcs.Task));
    catch
      InvalidOperationException => Console.WriteLine("blocked"),
    end try;
  end Main;
end Program;
