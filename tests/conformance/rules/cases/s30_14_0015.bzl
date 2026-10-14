//! case: s30_14_0015
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn admits an entry whose nested lambda uses only the entry's own local
//! rule: The entry must be capture-free: a non-capturing lambda or a static
//!   method reference. A capturing lambda is one that references a variable of
//!   its enclosing scope (v0.338 §27.5). A local the entry declares itself is
//!   not captured by the entry. (v0.338 §30.14, §27.5)
//! cites: 30.14, 27.5
//! expect: accept
module Conf;

import System.Threading;

public class Program
  public method Run()
    using var t := Domain.Spawn(lambda() => do
      var y := 2;
      var g := lambda(x: int) => x + y;
      Console.WriteLine(g(1));
    end);
  end Run;
end Program;
