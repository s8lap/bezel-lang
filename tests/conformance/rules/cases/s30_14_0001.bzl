//! case: s30_14_0001
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn rejects a capturing lambda entry
//! rule: The compiler enforces at the call site: **the entry must be
//!   capture-free**: a non-capturing lambda or a static method reference. A
//!   capturing lambda or an instance method reference is rejected —
//!   captures and receivers are borrows of the origin domain.
//! cites: 30.14
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public method Run()
    var n := 5;
    using var t := Domain.Spawn(lambda() => Console.WriteLine(n));
  end Run;
end Program;
