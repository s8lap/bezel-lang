//! case: s5_1_0168
//! doc: spec
//! section: 5.1
//! title: a uint argument at the Semaphore int initialCount parameter is rejected
//! rule: Between signed and unsigned types of the same or narrower width
//! cites: 5.1
//! expect: reject
//! diagnostic: not implicitly convertible to the target type
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var v: uint := 5;
    var s := new Semaphore(v);
    Console.WriteLine(1);
  end Main;
end Program;
