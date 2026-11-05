//! case: l6_3_1301
//! doc: stdlib
//! section: 6.3
//! title: a SemaphoreToken disposed through an IDisposable parameter returns its permit
//! rule: disposing the token returns it
//! cites: 6.3, 28.2
//! expect: run
//! output: reacquired
module Conf;

import System.Threading;

public class Program
  static method Release(owned d: IDisposable)
    d.Dispose();
  end Release;

  public static method Main()
    using var s := new Semaphore(1);
    Release(s.Acquire());
    using var again := s.TryAcquire(TimeSpan.FromMilliseconds(500));
    if again <> null then
      Console.WriteLine("reacquired");
    else
      Console.WriteLine("blocked");
    end;
  end Main;
end Program;
