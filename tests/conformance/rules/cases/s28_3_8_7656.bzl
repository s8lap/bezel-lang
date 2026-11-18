//! case: s28_3_8_7656
//! doc: spec
//! section: 28.3.8
//! title: a capture-free lambda literal passed to a generic method that stores it keeps working
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3.8, 28.9
//! expect: run
//! output: 600
//!   ran
module Conf;
import System;
public class Box<T>
  public F: T?;
  public constructor() end constructor;
  public method Keep(x: T) F := x; end Keep;
end Box;
public class Program
  public static method Main(): int
    var sum := 0;
    for var i in 0 ..< 200 do
      using var b := new Box<() -> int>();
      b.Keep(lambda() => 3);
      sum := sum + b.F!();
    end for;
    Console.WriteLine(sum);
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
