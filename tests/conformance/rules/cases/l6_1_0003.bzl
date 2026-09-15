//! case: l6_1_0003
//! doc: stdlib
//! section: 6.1
//! title: ThrowIfCancellationRequested raises OperationCanceledException once cancelled
//! rule: public method ThrowIfCancellationRequested(); — see language
//!   spec §30.8 cancellation.
//! cites: 6.1
//! expect: run
//! stdout: caught
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    try
      cts.Token.ThrowIfCancellationRequested();
    catch
      OperationCanceledException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
