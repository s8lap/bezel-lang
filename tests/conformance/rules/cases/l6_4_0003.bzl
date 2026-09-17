//! case: l6_4_0003
//! doc: stdlib
//! section: 6.4
//! title: AtomicInteger<T>.Add raises OverflowException on overflow
//! rule: Increment, Decrement, and Add perform the operation atomically
//!   ... an operation whose mathematical result is not representable in
//!   T raises OverflowException.
//! cites: 6.4
//! expect: run
//! stdout: caught
module Conf;

import System.Threading;

public class Program
  public static method Main()
    var a := new AtomicInteger<int>(int.MaxValue);
    try
      _ := a.Add(1);
    catch
      OverflowException => Console.WriteLine("caught")
    end;
  end Main;
end Program;
