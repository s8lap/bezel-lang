//! case: s30_14_0016
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn admits a capture-free async entry that uses its own local
//! rule: An entry may be an `async` function: a capture-free `async` lambda or
//!   a static `async` method reference. (v0.338 §30.14). A local the entry
//!   declares itself is not a capture (v0.338 §27.5).
//! cites: 30.14, 30.6, 27.5
//! issue: 1523
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public method Run()
    using var t := Domain.Spawn(async lambda() => do
      var z := 3;
      await Task.Delay(1);
      Console.WriteLine(z);
    end);
  end Run;
end Program;
