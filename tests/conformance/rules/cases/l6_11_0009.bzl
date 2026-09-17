//! case: l6_11_0009
//! doc: stdlib
//! section: 6.11
//! title: a WriteGuard's Value carries no store restriction
//! rule: WriteGuard<T>.Value and LockGuard<T>.Value carry no restriction —
//!   those guards are exclusive.
//! cites: 6.11, 30.12
//! expect: run
//! stdout: 5
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new RwLocked<Counter>(new Counter(0));
    using var g := lk.Write();
    g.Value.N := 5;
    Console.WriteLine(g.Value.N);
  end Main;
end Program;
