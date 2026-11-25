//! case: s22_3_9404
//! doc: spec
//! section: 22.3
//! title: a ref argument naming a function-typed field aliases the field across a call and a write
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.1
//! expect: run
//! output: 4400
module Conf;

public class Holder
  public f: () -> int := lambda() => 4;
end Holder;

public class Program
  public static method Peek(ref x: () -> int): int => x();
  public static method Swap(ref x: () -> int): int
    x := lambda() => 9;
    return x();
  end Swap;
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      using var h := new Holder();
      s := s + Peek(ref h.f) + Swap(ref h.f) + h.f();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
