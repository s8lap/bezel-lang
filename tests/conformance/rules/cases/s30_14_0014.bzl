//! case: s30_14_0014
//! doc: spec
//! section: 30.14
//! title: an async Domain.Spawn entry that captures a method local is rejected
//! rule: The entry must be capture-free: a non-capturing lambda or a static
//!   method reference. (v0.338 §30.14). An entry may be an `async` function: a
//!   capture-free `async` lambda or a static `async` method reference.
//! cites: 30.14, 30.6
//! expect: reject
//! diagnostic: capture
module Conf;

import System.Threading;

public class Program
  public method Run()
    var n := 5;
    using var t := Domain.Spawn(async lambda() => do
      await Task.Delay(1);
      Console.WriteLine(n);
    end);
  end Run;
end Program;
