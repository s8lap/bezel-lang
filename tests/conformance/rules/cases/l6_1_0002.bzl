//! case: l6_1_0002
//! doc: stdlib
//! section: 6.1
//! title: Register on an already-cancelled token fires the callback synchronously
//! rule: If the token is already in the cancelled state at the moment
//!   Register is called, the callback is invoked synchronously on the
//!   calling thread before Register returns.
//! cites: 6.1
//! expect: run
//! output: fired
//!   after
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    using var sub := cts.Token.Register(lambda() => Console.WriteLine("fired"));
    Console.WriteLine("after");
  end Main;
end Program;
