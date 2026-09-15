//! case: l6_11_0005
//! doc: stdlib
//! section: 6.11
//! title: an explicit Dispose() of a live guard binding is rejected while a borrow of its Value is live
//! rule: a guard binding cannot be explicitly Dispose()d or discarded
//!   while a borrow from its Value is live.
//! cites: 6.11, 30.12
//! expect: reject
//! diagnostic: live
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
    g.Dispose();
    Console.WriteLine(c.N);
  end Main;
end Program;
