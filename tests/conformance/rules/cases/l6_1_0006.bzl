//! case: l6_1_0006
//! doc: stdlib
//! section: 6.1
//! title: Register on an unpublished source invokes the callback on Cancel
//! rule: Register invokes the callback when cancellation is requested; the
//!   returned Subscription removes the registration when disposed.
//! cites: 6.1
//! expect: run
//! output: fired
//!   after
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    using var reg := cts.Token.Register(lambda() => Console.WriteLine("fired"));
    cts.Cancel();
    Console.WriteLine("after");
  end Main;
end Program;
