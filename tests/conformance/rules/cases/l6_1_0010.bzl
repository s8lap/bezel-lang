//! case: l6_1_0010
//! doc: stdlib
//! section: 6.1
//! title: Register on a linked child of a published parent raises
//! rule: A linked child of a published parent is published; on a published
//!   source, Register raises InvalidOperationException.
//! cites: 6.1
//! expect: run
//! output: caught
module Conf;

import System.Threading;

public static class Registry
  private static owned readonly _cts: CancellationTokenSource := new();
  public static property Token: CancellationToken => _cts.Token;
end Registry;

public class Program
  public static method Main()
    using var child := CancellationTokenSource.CreateLinked(Registry.Token, CancellationToken.None);
    try
      using var sub := child.Token.Register(lambda() => Console.WriteLine("never"));
    catch
      InvalidOperationException => Console.WriteLine("caught")
    end try;
  end Main;
end Program;
