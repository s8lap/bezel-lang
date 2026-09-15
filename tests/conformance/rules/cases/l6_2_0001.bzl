//! case: l6_2_0001
//! doc: stdlib
//! section: 6.2
//! title: TryAcquire succeeds immediately on an uncontended Mutex
//! rule: TryAcquire(timeout) returns owned Lock? — an owned Lock on
//!   success or null on timeout.
//! cites: 6.2, 1
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var mutex := new Mutex();
    using var guard := mutex.TryAcquire(TimeSpan.FromSeconds(1));
    Console.WriteLine(guard <> null);
  end Main;
end Program;
