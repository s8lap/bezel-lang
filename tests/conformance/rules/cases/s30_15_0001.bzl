//! case: s30_15_0001
//! doc: spec
//! section: 30.15
//! title: Channel<T> rejects a non-sendable class-typed element
//! rule: Channels are the only mutable objects reachable from more than one
//!   domain; they are compiler-known, internally synchronized, and their
//!   element type must be sendable (checked at instantiation per §3.6).
//! cites: 30.15, 30.14
//! expect: reject
//! diagnostic: sendable
module Conf;

import System.Threading;

public class Report
  public constructor()
  end constructor;
end Report;

public class Program
  public method Run()
    using var inbox := new Channel<Report>();
  end Run;
end Program;
