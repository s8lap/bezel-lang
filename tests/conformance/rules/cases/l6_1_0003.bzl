//! case: l6_1_0003
//! doc: stdlib
//! section: 6.1
//! title: ThrowIfCancellationRequested raises OperationCanceledException once cancelled
//! rule: Cancellation surfaces as OperationCanceledException (language spec
//!   §30.8); the generated async iterator's implicit
//!   token.ThrowIfCancellationRequested() "faults the in-flight
//!   MoveNextAsync task with OperationCanceledException" on a requested
//!   cancellation (language spec §30.7), which is the call's own effect.
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
