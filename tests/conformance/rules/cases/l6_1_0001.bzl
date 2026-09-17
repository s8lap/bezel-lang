//! case: l6_1_0001
//! doc: stdlib
//! section: 6.1
//! title: CancellationTokenSource.IsCancellationRequested agrees with its Token's property
//! rule: CancellationTokenSource.IsCancellationRequested and
//!   source.Token.IsCancellationRequested always return the same value:
//!   the source's property is a convenience equivalent to
//!   source.Token.IsCancellationRequested.
//! cites: 6.1
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    Console.WriteLine(cts.IsCancellationRequested = cts.Token.IsCancellationRequested);
  end Main;
end Program;
