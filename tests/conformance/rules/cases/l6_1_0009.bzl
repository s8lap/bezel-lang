//! case: l6_1_0009
//! doc: stdlib
//! section: 6.1
//! title: cancelling a parent cancels the linked source
//! rule: The linked source enters the cancelled state when any parent token
//!   cancels or its own Cancel()/CancelAfter() fires, whichever happens first.
//! cites: 6.1
//! expect: run
//! output: False
//!   True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var parent := new CancellationTokenSource();
    using var child := CancellationTokenSource.CreateLinked(parent.Token, CancellationToken.None);
    Console.WriteLine(child.IsCancellationRequested);
    parent.Cancel();
    Console.WriteLine(child.IsCancellationRequested);
  end Main;
end Program;
