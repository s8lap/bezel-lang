//! case: s22_3_9407
//! doc: spec
//! section: 22.3
//! title: a function-typed out parameter is callable after assignment and in the caller after return
//! rule: `out` is **copy-out-on-normal-return**: within the callee the parameter denotes a
//!   callee-local slot, freely readable after assignment; on normal return the slot's value is
//!   copied to the caller's recipient
//! cites: 22.3, 27.1
//! expect: run
//! output: 2000
module Conf;

public class Program
  public static method Make(out x: () -> int): int
    x := lambda() => 5;
    return x();
  end Make;
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      var f: () -> int;
      s := s + Make(out f) + f();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
