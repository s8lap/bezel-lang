//! case: l6_2_1301
//! doc: stdlib
//! section: 6.2
//! title: a Lock disposed through an IDisposable parameter releases its Mutex
//! rule: releases the held lock
//! cites: 6.2, 28.2
//! expect: run
//! output: reacquired
module Conf;

import System.Threading;

public class Program
  static method Release(owned d: IDisposable)
    d.Dispose();
  end Release;

  public static method Main()
    using var m := new Mutex();
    Release(m.Acquire());
    using var again := m.TryAcquire(TimeSpan.FromMilliseconds(500));
    if again <> null then
      Console.WriteLine("reacquired");
    else
      Console.WriteLine("blocked");
    end;
  end Main;
end Program;
