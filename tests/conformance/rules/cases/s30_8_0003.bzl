//! case: s30_8_0003
//! doc: spec
//! section: 30.8
//! title: tokens obtained before Cancel() observe the source's later state
//! rule: The token observes the source's state; when `Cancel()` is called on
//!   the source, all tokens derived from it report `IsCancellationRequested ==
//!   true`.
//! cites: 30.8, 9.7
//! expect: run
//! output: False False
//!   True True
//! notes: s30_8_0002 reads a token taken after Cancel(); here both tokens are
//!   copied out first, so a snapshot-on-copy token would print False twice.
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    var a := cts.Token;
    var b := cts.Token;
    Console.WriteLine($"{a.IsCancellationRequested} {b.IsCancellationRequested}");
    cts.Cancel();
    Console.WriteLine($"{a.IsCancellationRequested} {b.IsCancellationRequested}");
  end Main;
end Program;
