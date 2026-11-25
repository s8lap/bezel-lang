//! case: s27_5_9401
//! doc: spec
//! section: 27.5
//! title: a lambda writing a captured function-typed ref parameter is seen by a direct call of the parameter
//! rule: The capture names the parameter's own location, not a copy: a `ref` parameter aliases the
//!   caller's location, so a write through the closure is a write to it
//! cites: 27.5, 22.3
//! expect: run
//! output: 220000
module Conf;

public class Program
  public static method Peek(ref x: () -> int): int
    var g: () -> int := lambda() => do x := lambda() => 50; return 0; end;
    var a := x();
    var z := g();
    var b := x();
    return a * 1000 + b + z;
  end Peek;
  public static method Main(): int
    var s := 0;
    for var i in 0 ..< 200 do
      var f: () -> int := lambda() => 1;
      s := s + Peek(ref f) + f();
    end for;
    Console.WriteLine(s);
    return 0;
  end Main;
end Program;
