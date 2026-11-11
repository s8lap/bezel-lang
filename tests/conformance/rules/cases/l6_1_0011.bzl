//! case: l6_1_0011
//! doc: stdlib
//! section: 6.1
//! title: TimeoutException is a failure an IFailure arm takes
//! rule: The failure types this document declares are `IOException` and the
//!   classes deriving from it (§7.5), `TimeoutException` (§6.1),
//!   `ChannelClosedException` and `ChannelFullException` (§6.9), and
//!   `DomainFailedException` (§6.8) … no built-in class is one.
//! cites: 6.1, 1
//! expect: run
//! stdout: deadline
//! issue: 8414
module Conf;

import System.Threading;

public class Program
  public static method Main()
    try
      throw new TimeoutException("deadline");
    catch
      var f: IFailure => Console.WriteLine(f.Message)
    end;
  end Main;
end Program;
