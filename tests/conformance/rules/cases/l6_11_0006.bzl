//! case: l6_11_0006
//! doc: stdlib
//! section: 6.11
//! title: disposing a guard after its last interior borrow use is admitted
//! rule: a guard binding cannot be explicitly Dispose()d or discarded
//!   while a borrow from its Value is live.
//! cites: 6.11, 30.12
//! expect: run
//! stdout: 0
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new Locked<Counter>(new Counter(0));
    var g := lk.Lock();
    var c := g.Value;
    Console.WriteLine(c.N);
    g.Dispose();
  end Main;
end Program;
