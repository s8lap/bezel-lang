//! case: l6_4_0001
//! doc: stdlib
//! section: 6.4
//! title: Atomic<T>.Exchange atomically replaces Value and returns the previous value
//! rule: Exchange(value) atomically replaces Value with value and
//!   returns the previous value.
//! cites: 6.4
//! expect: run
//! stdout: 5
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var a := new Atomic<int>(5);
    Console.WriteLine(a.Exchange(9));
  end Main;
end Program;
