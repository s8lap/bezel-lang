//! case: s30_12_0008
//! doc: spec
//! section: 30.12
//! title: a blocking Acquire that would park inside a domain continuation raises InvalidOperationException
//! rule: where static rejection cannot see the call (a non-`async` helper
//!   reached from a continuation), the runtime raises
//!   `InvalidOperationException` when the acquisition would actually park a
//!   thread inside a domain continuation
//! cites: 30.12, 30.13
//! expect: run
//! output: refused
//!   done
//! notes: the continuation already holds the mutex (non-reentrant: a holder
//!   re-acquiring waits on itself), so the helper's Acquire would park; it
//!   must raise rather than hang. Twin of s30_12_0007.
module Conf;

import System.Threading;

public class Program
  public static method Critical(m: Mutex)
    using var g := m.Acquire();
    Console.WriteLine("locked");
  end Critical;

  public static async method Main(): Task
    using var m := new Mutex();
    using var held := await m.AcquireAsync();
    try
      Critical(m);
    catch
      InvalidOperationException => Console.WriteLine("refused"),
    end try;
    Console.WriteLine("done");
  end Main;
end Program;
