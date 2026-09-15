//! case: l6_4_0002
//! doc: stdlib
//! section: 6.4
//! title: AtomicInteger<T>.Increment returns the post-increment value
//! rule: Increment(): T; // returns post-increment value
//! cites: 6.4
//! expect: run
//! stdout: 6
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var a := new AtomicInteger<int>(5);
    Console.WriteLine(a.Increment());
  end Main;
end Program;
