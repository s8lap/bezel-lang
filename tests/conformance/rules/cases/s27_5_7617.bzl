//! case: s27_5_7617
//! doc: spec
//! section: 27.5
//! title: method references used inside two lambda arguments yield the hand-computed total
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5, 27.6
//! expect: run
//! stdout: 16400
module Conf;
import System;
public static class Program
  public static method Inc(x: int): int => x + 1;
  public static method Id<T>(x: T): T
    var k: int := 1;
    var h := lambda() => k;
    var z := h();
    return x;
  end;
  public static method Ap(p: (int) -> int, v: int): int => p(v);
  public static method Two(f: () -> int, g: () -> int): int => f() + g();
  public static method Main()
    var i: int := 0;
    var tot: int := 0;
    while i < 200 do
      var n: int := 40;
      tot := tot + Two(lambda() => Ap(Program.Inc, n), lambda() => Ap(Program.Id<int>, n + 1));
      i := i + 1;
    end;
    Console.WriteLine(tot.ToString());
  end Main;
end Program;
