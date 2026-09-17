//! case: l6_3_0001
//! doc: stdlib
//! section: 6.3
//! title: Semaphore.TryAcquire fails once the permit count is exhausted
//! rule: constructor(initialCount: int); ... TryAcquire(timeout: TimeSpan):
//!   owned SemaphoreToken?
//! cites: 6.3, 1
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var sem := new Semaphore(1);
    using var first := sem.Acquire();
    using var second := sem.TryAcquire(TimeSpan.Zero);
    Console.WriteLine(second = null);
  end Main;
end Program;
