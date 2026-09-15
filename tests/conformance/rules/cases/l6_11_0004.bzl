//! case: l6_11_0004
//! doc: stdlib
//! section: 6.11
//! title: TryLock(timeout) returns null while the lock is already held
//! rule: TryLock(timeout: TimeSpan): owned LockGuard<T>?; // null on
//!   timeout (Try shape (b)). Non-reentrant: a holder re-acquiring waits
//!   on itself.
//! cites: 6.11, 30.12
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public sealed class Counter
  public N: int;
  public constructor(n: int) N := n; end constructor;
end Counter;

public class Program
  public static method Main()
    using var lk := new Locked<Counter>(new Counter(0));
    using var held := lk.Lock();
    using var second := lk.TryLock(TimeSpan.Zero);
    Console.WriteLine(second = null);
  end Main;
end Program;
