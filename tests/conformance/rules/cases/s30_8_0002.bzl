//! case: s30_8_0002
//! doc: spec
//! section: 30.8
//! title: IsCancellationRequested observes a Cancel() call on the source
//! rule: `CancellationToken` observes the source's state; when `Cancel()`
//!   is called on the source, all tokens derived from it report
//!   `IsCancellationRequested == true`.
//! cites: 30.8
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var cts := new CancellationTokenSource();
    cts.Cancel();
    Console.WriteLine(cts.Token.IsCancellationRequested);
  end Main;
end Program;
