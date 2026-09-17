//! case: l6_11_0008
//! doc: stdlib
//! section: 6.11
//! title: a store through a ReadGuard-sourced borrow is rejected
//! rule: In the body holding the guard, the compiler rejects all stores,
//!   move, and ref/out passing through a ReadGuard-sourced borrow.
//! cites: 6.11, 30.12
//! expect: reject
//! diagnostic: read
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new RwLocked<Counter>(new Counter(0));
    using var g := lk.Read();
    g.Value.N := 5;
  end Main;
end Program;
