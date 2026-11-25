//! case: s22_3_9401
//! doc: spec
//! section: 22.3
//! title: a bare method group local called with a ref argument writes through the caller's variable
//! rule: A `ref` argument naming a mutable local or writable field is a true alias: callee writes are
//!   visible through the caller's location immediately
//! cites: 22.3, 27.6, 27.1
//! expect: run
//! output: 2
module Conf;

public class Program
  static method Inc(ref x: int)
    x := x + 1;
  end Inc;
  public static method Main(): int
    var f := Inc;
    var a := 1;
    f(ref a);
    Console.WriteLine(a);
    return 0;
  end Main;
end Program;
