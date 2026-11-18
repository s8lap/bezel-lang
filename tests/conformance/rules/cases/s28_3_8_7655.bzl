//! case: s28_3_8_7655
//! doc: spec
//! section: 28.3.8
//! title: a capture-free lambda literal passed to a generic constructor that stores it is admitted
//! rule: At every set-erasing position a function value with a non-empty source set is rejected, and a capture-free one is admitted
//! cites: 28.3.8, 28.9
//! expect: run
//! output: 600
//!   ran
module Conf;
import System;
public class Keep<T>
  public F: T?;
  public constructor(a: T) F := a; end constructor;
end Keep;
public class Program
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      using var k := new Keep<() -> int>(lambda() => 3);
      s := s + k.F!();
    end for;
    Console.WriteLine(s);
    Console.WriteLine("ran");
    return 0;
  end Main;
end Program;
