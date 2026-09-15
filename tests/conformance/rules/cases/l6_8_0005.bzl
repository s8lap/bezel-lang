//! case: l6_8_0005
//! doc: stdlib
//! section: 6.8
//! title: a sendable value type crosses the spawn boundary as a copy
//! rule: A CancellationToken never crosses a domain boundary itself: it is
//!   a borrow-bearing record and not sendable.
//! cites: 6.8, 30.14
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public static method Poll(n: int)
    Console.WriteLine(n);
  end Poll;

  public method Run()
    using var t := Domain.Spawn(Program.Poll, 5);
  end Run;
end Program;
