//! case: s30_14_0011
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn rejects an entry whose nested lambda captures an enclosing local
//! rule: The compiler enforces at the call site: the entry must be
//!   capture-free: a non-capturing lambda or a static method reference. A
//!   capturing lambda or an instance method reference is rejected. (v0.338
//!   §30.14). A lambda may reference variables from its enclosing scope (§27.5),
//!   so an entry containing a lambda that references a method local captures it.
//! cites: 30.14, 27.5
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public method Run()
    var n := 5;
    using var t := Domain.Spawn(lambda() => do
      var f := lambda() => Console.WriteLine(n);
      f();
    end);
  end Run;
end Program;
