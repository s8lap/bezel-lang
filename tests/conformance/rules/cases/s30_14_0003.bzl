//! case: s30_14_0003
//! doc: spec
//! section: 30.14
//! title: Domain.Spawn admits a static method reference entry
//! rule: The entry must be capture-free: a non-capturing lambda or a
//!   static method reference.
//! cites: 30.14
//! expect: accept
module Conf;

import System.Threading;

public class Worker
  public static method Greet()
    Console.WriteLine("hi");
  end Greet;
end Worker;

public class Program
  public method Run()
    using var t := Domain.Spawn(Worker.Greet);
  end Run;
end Program;
