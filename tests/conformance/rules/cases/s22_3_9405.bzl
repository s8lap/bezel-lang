//! case: s22_3_9405
//! doc: spec
//! section: 22.3
//! title: a ref argument naming a function-typed array element aliases the element
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.1
//! expect: run
//! output: 1000
module Conf;

public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Main(): int
    var s := 0;
    var arr: Array<() -> int> := [lambda() => 0, lambda() => 0, lambda() => 0];
    arr[1] := lambda() => 5;
    for var i in 0 ..< 200 do
      s := s + Peek(ref arr[1]);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
