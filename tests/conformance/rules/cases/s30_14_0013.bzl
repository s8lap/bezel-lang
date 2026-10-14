//! case: s30_14_0013
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn rejects an entry that captures a for-loop variable
//! rule: The compiler enforces at the call site: the entry must be
//!   capture-free: a non-capturing lambda or a static method reference. A
//!   capturing lambda or an instance method reference is rejected. (v0.338
//!   §30.14)
//! cites: 30.14
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public method Run()
    for var i in 0..<2 do
      using var t := Domain.Spawn(lambda() => Console.WriteLine(i));
    end;
  end Run;
end Program;
