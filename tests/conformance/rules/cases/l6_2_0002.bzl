//! case: l6_2_0002
//! doc: stdlib
//! section: 6.2
//! title: TryAcquire(TimeSpan.Zero) returns null while the mutex is already held
//! rule: TryAcquire(timeout) returns owned Lock? — an owned Lock on
//!   success or null on timeout (§6.2). Mutex is non-reentrant — "a
//!   holder re-acquiring waits on itself, whatever the acquisition form"
//!   — is language spec §30.12's sentence, not §6.2's.
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
