//! case: s30_12_0012
//! doc: spec
//! section: 30.12
//! title: disposing the guard before the lock binding is admitted
//! rule: For the same body-local analysis, a live guard counts as a live local
//!   borrow of the lock binding it was acquired from: consuming, discarding,
//!   or explicitly disposing a lock binding while a guard obtained from it is
//!   live is rejected the same way.
//! cites: 30.12, 28.3
//! expect: run
//! stdout: 1
//! issue: 1721
//! notes: accepting twin of s30_12_0011.
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    var lk := new Locked<Counter>(new Counter(0));
    var g := lk.Lock();
    g.Value.N := 1;
    Console.WriteLine(g.Value.N);
    g.Dispose();
    lk.Dispose();
  end Main;
end Program;
