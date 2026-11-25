//! case: s22_3_9403
//! doc: spec
//! section: 22.3
//! title: a function-typed ref parameter holding a capturing lambda is called through the ref slot
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.1, 27.3
//! expect: run
//! output: 20100
module Conf;

public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      var k := i;
      var f: () -> int := lambda() => k + 1;
      s := s + Peek(ref f);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
