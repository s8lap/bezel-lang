//! case: s22_3_9406
//! doc: spec
//! section: 22.3
//! title: a function-typed ref parameter forwarded as a ref argument keeps the alias
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.1
//! expect: run
//! output: 3200
module Conf;

public class Program
  public static method Inner(ref y: () -> int): int
    return y();
  end Inner;
  public static method Outer(ref x: () -> int): int
    x := lambda() => 8;
    return Inner(ref x) + x();
  end Outer;
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 1;
      s := s + Outer(ref f);
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
