//! case: l6_1_0004
//! doc: stdlib
//! section: 6.1
//! title: Register on a published static source raises InvalidOperationException
//! rule: A source is published at a static readonly field of the type;
//!   on a published source, Register raises InvalidOperationException.
//! cites: 6.1
//! expect: run
//! stdout: caught
module Conf;

import System.Threading;

public static class Registry
  private static owned readonly _cts: CancellationTokenSource := new();
  public static property Token: CancellationToken => _cts.Token;
end Registry;

public class Program
  public static method Main()
    try
      using var sub := Registry.Token.Register(lambda() => Console.WriteLine("never"));
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
