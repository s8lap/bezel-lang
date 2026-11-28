//! case: l6_7_3601
//! doc: stdlib
//! section: 6.7
//! title: disposing the token source cancels a pending Task.Delay at the disposal
//! rule: a cancellation that comes from the source's **disposal** counts, disposal being a full
//!   `Cancel()` (§6.1): a `using var` source leaving scope while a delay on its token is pending cancels
//!   the delay
//! cites: 6.7, 6.1
//! expect: run
//! stdout: OCE
//! issue: 8486
//! notes: The delay is 5 s; it completes at the disposal, not at the deadline.
module Conf;

import System.Threading;

public class Program
  public static async method Main(): Task
    var cts := new CancellationTokenSource();
    var t := Task.Delay(5000, cts.Token);
    cts.Dispose();
    try
      await t;
      Console.WriteLine("elapsed");
    catch
      OperationCanceledException => Console.WriteLine("OCE"),
    end try;
  end Main;
end Program;
