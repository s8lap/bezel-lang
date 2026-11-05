//! case: s12_7_1302
//! doc: spec
//! section: 12.7
//! title: is-tests at an Object carrier answer true for a held Lock, a Semaphore and a SemaphoreToken
//! rule: `is T` tests whether a value is an instance of `T` and produces `bool`
//! cites: 8.2, 19.3
//! expect: run
//! output: Lock
//!   SemaphoreToken
//!   Semaphore
//!   other
module Conf;

import System.Threading;

public class Program
  static method Kind(o: Object): string
    if o is Lock then
      return "Lock";
    end;
    if o is SemaphoreToken then
      return "SemaphoreToken";
    end;
    if o is Semaphore then
      return "Semaphore";
    end;
    return "other";
  end Kind;

  public static method Main()
    using var m := new Mutex();
    using var s := new Semaphore(1);
    using var l := m.Acquire();
    using var t := s.Acquire();
    Console.WriteLine(Kind(l));
    Console.WriteLine(Kind(t));
    Console.WriteLine(Kind(s));
    Console.WriteLine(Kind(m));
  end Main;
end Program;
