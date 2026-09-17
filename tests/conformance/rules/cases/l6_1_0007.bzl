//! case: l6_1_0007
//! doc: stdlib
//! section: 6.1
//! title: disposing a source performs a full Cancel before severing registrations
//! rule: Disposal performs a full Cancel() (v0.195): registered callbacks that
//!   have not yet run are invoked synchronously on the disposing thread before
//!   the registration list is severed.
//! cites: 6.1
//! expect: run
//! output: fired
//!   after
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var cts := new CancellationTokenSource();
    using var reg := cts.Token.Register(lambda() => Console.WriteLine("fired"));
    cts.Dispose();
    Console.WriteLine("after");
  end Main;
end Program;
