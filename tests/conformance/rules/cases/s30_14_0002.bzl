//! case: s30_14_0002
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn admits a non-capturing lambda entry
//! rule: The entry must be capture-free: a non-capturing lambda or a
//!   static method reference.
//! cites: 30.14
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public method Run()
    using var t := Domain.Spawn(lambda() => Console.WriteLine("hi"));
  end Run;
end Program;
