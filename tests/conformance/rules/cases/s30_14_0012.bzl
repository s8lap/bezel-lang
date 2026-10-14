//! case: s30_14_0012
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn rejects an entry that captures a method parameter
//! rule: The compiler enforces at the call site: the entry must be
//!   capture-free: a non-capturing lambda or a static method reference. A
//!   capturing lambda or an instance method reference is rejected. (v0.338
//!   §30.14). Data enters the new domain only through the explicit `arg`
//!   parameters.
//! cites: 30.14
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public method Run(n: int)
    using var t := Domain.Spawn(lambda() => Console.WriteLine(n));
  end Run;
end Program;
