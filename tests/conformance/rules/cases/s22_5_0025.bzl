//! case: s22_5_0025
//! doc: spec
//! section: 22.5
//! title: a Semaphore constructed with reordered named arguments holds initialCount permits
//! rule: A call's supplied argument expressions are evaluated in call-site textual order, left to right as
//!   written, for positional and named arguments alike; naming an argument changes which parameter it
//!   binds to, never when it runs. Naming binds `initialCount: 2` and
//!   `maxCount: 5`, so exactly two permits are available and a third acquisition cannot succeed.
//! cites: 22.5, 22.6
//! expect: run
//! output: exhausted
//!   True
module Conf;

import System.Threading;

public class Program
  public static method Main()
    using var s := new Semaphore(maxCount := 5, initialCount := 2);
    using var t1 := s.Acquire();
    using var t2 := s.Acquire();
    using var t3 := s.TryAcquire(TimeSpan.FromMilliseconds(1.0));
    if t3 = null then
      Console.WriteLine("exhausted");
    else
      Console.WriteLine("extra permit");
    end;
    using var a := new Semaphore(initialCount := 1, maxCount := 5);
    using var b := new Semaphore(1, 5);
    using var ta := a.Acquire();
    using var tb := b.Acquire();
    using var ra := a.TryAcquire(TimeSpan.FromMilliseconds(1.0));
    using var rb := b.TryAcquire(TimeSpan.FromMilliseconds(1.0));
    Console.WriteLine((ra = null) and (rb = null));
  end Main;
end Program;
