//! case: s30_12_0009
//! doc: spec
//! section: 30.12
//! title: a Semaphore holder acquiring again consumes a second permit rather than waiting on itself
//! rule: (`Semaphore` is a counting primitive: a second `Acquire()` by the
//!   same holder consumes another permit — not reentrancy, and legal while
//!   permits remain.)
//! cites: 30.12
//! expect: run
//! output: two
//!   True
//! notes: on the startup thread (non-async Main), so the blocking forms are
//!   legal; the zero-timeout TryAcquire shows both permits were consumed.
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var sem := new Semaphore(2);
    using var a := sem.Acquire();
    using var b := sem.Acquire();
    Console.WriteLine("two");
    using var c := sem.TryAcquire(TimeSpan.Zero);
    Console.WriteLine(c = null);
  end Main;
end Program;
