//! case: l6_3_0002
//! doc: stdlib
//! section: 6.3
//! title: releasing a SemaphoreToken restores a permit for the next Acquire
//! rule: SemaphoreToken — destructor; // releases the held permit
//! cites: 6.3
//! expect: run
//! stdout: True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var sem := new Semaphore(1);
    do
      using var first := sem.Acquire();
    end;
    using var second := sem.TryAcquire(TimeSpan.Zero);
    Console.WriteLine(second <> null);
  end Main;
end Program;
