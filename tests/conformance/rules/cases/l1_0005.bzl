//! case: l1_0005
//! doc: stdlib
//! section: 1
//! title: Try shape (c) assigns default(T) to the out parameter on the false path
//! rule: Try shape (c) -- bool + non-nullable value-typed out -- assigns
//!   default(T) to the out parameter when the operation declines. Convert.TryToInt32
//!   is this shape over a concrete value-type payload.
//! cites: 2.3
//! expect: run
//! stdout: 0
module Conf;

public class Program
  public static method Main()
    var ok := Convert.TryToInt32("not a number", out var result);
    Console.WriteLine(result);
  end Main;
end Program;
