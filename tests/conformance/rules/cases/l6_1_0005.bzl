//! case: l6_1_0005
//! doc: stdlib
//! section: 6.1
//! title: CancellationToken.None is never cancelled and never raises
//! rule: CancellationToken.None has no source and is never in the cancelled
//!   state, so ThrowIfCancellationRequested never raises on it.
//! cites: 6.1
//! expect: run
//! output: False
module Conf;

public class Program
  public static method Main()
    CancellationToken.None.ThrowIfCancellationRequested();
    Console.WriteLine(CancellationToken.None.IsCancellationRequested);
  end Main;
end Program;
