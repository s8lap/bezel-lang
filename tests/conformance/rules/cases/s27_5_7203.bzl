//! case: s27_5_7203
//! doc: spec
//! section: 27.5
//! title: an aliased closure argument stored from a lambda stays callable
//!   after the callee returns
//! rule: Captured variables are heap-allocated in a closure object
//! cites: 27.5, 28.9
//! expect: run
//! output: 20100
//!   8400
module Conf;

public class Program
  public static method AddTo<U>(l: Array<U>, x: U, i: int): int
    var a := x;
    var k: () -> int := lambda() => do
      l.Add(a);
      return 1;
    end;
    return k() + i;
  end AddTo;

  public static method Main()
    var keep := new Array<() -> int>();
    var total := 0;
    for var i in 0..<200 do
      total := total + Program.AddTo<() -> int>(keep, lambda() => 42, i);
    end for;
    var s := 0;
    for var g in keep do
      s := s + g();
    end for;
    Console.WriteLine(total);
    Console.WriteLine(s);
  end Main;
end Program;
