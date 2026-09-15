//! case: l6_2_0002
//! doc: stdlib
//! section: 6.2
//! title: TryAcquire(TimeSpan.Zero) returns null while the mutex is already held
//! rule: TryAcquire(timeout) returns owned Lock? — an owned Lock on
//!   success or null on timeout; Mutex is non-reentrant.
//! cites: 6.2, 1
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var mutex := new Mutex();
    using var held := mutex.Acquire();
    using var second := mutex.TryAcquire(TimeSpan.Zero);
    Console.WriteLine(second = null);
  end Main;
end Program;
