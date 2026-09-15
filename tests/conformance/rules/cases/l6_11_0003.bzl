//! case: l6_11_0003
//! doc: stdlib
//! section: 6.11
//! title: a lock guard's Value is a mutable borrow of the interior
//! rule: A guard's Value property is a borrow of the interior whose source
//!   is the guard ... so the borrow rules bound every interior borrow by
//!   the guard's life.
//! cites: 6.11, 30.12
//! expect: run
//! stdout: 1
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new Locked<Counter>(new Counter(0));
    using var g := lk.Lock();
    g.Value.N := g.Value.N + 1;
    Console.WriteLine(g.Value.N);
  end Main;
end Program;
