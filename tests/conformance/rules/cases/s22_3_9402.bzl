//! case: s22_3_9402
//! doc: spec
//! section: 22.3
//! title: a write to a function-typed ref parameter is seen by a later call through it and the caller
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.1
//! expect: run
//! output: 2800
module Conf;

public class Program
  public static method Swap(ref x: () -> int): int
    x := lambda() => 7;
    return x();
  end Swap;
  public static method Main(): int
    var f: () -> int := lambda() => 1;
    var s := 0;
    for var i in 0 ..< 200 do
      f := lambda() => 1;
      s := s + Swap(ref f) + f();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
