//! case: l6_11_0010
//! doc: stdlib
//! section: 6.11
//! title: a write guard's mutation is observed by a later read acquisition
//! rule: public method Read(): owned ReadGuard<T>; ... public method
//!   Write(): owned WriteGuard<T>;
//! cites: 6.11, 30.12, 30.13
//! expect: run
//! stdout: 42
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new RwLocked<Counter>(new Counter(0));
    do
      using var w := lk.Write();
      w.Value.N := 42;
    end;
    using var r := lk.Read();
    Console.WriteLine(r.Value.N);
  end Main;
end Program;
