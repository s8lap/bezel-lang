//! case: l6_8_0004
//! doc: stdlib
//! section: 6.8
//! title: CancellationToken cannot cross a domain boundary as a spawn argument
//! rule: A CancellationToken never crosses a domain boundary itself: it is
//!   a borrow-bearing record and not sendable.
//! cites: 6.8, 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public class Program
  public static method Poll(token: CancellationToken)
    Console.WriteLine(token.IsCancellationRequested);
  end Poll;

  public method Run()
    using var t := Domain.Spawn(Program.Poll, CancellationToken.None);
  end Run;
end Program;
