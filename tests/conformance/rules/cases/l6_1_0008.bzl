//! case: l6_1_0008
//! doc: stdlib
//! section: 6.1
//! title: a parent already cancelled at creation cancels the linked source at once
//! rule: A parent already cancelled at creation cancels the linked source
//!   before CreateLinked returns — the same race-free synchronous policy as
//!   Register on a cancelled token.
//! cites: 6.1
//! expect: run
//! output: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var parent := new CancellationTokenSource();
    parent.Cancel();
    using var child := CancellationTokenSource.CreateLinked(parent.Token, CancellationToken.None);
    Console.WriteLine(child.IsCancellationRequested);
  end Main;
end Program;
