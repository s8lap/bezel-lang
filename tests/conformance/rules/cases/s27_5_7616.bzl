//! case: s27_5_7616
//! doc: spec
//! section: 27.5
//! title: a predicate lambda argument, then a two-lambda call in a later method, gives the hand total
//! rule: A lambda may reference variables from its enclosing scope.
//! cites: 27.5
//! expect: run
//! stdout: 3200
module Conf;
import System;
public static class Program
  public static method A(xs: Array<int>): int
    var n: int := 3;
    var r: int := 0;
    if xs.Any(lambda(x) => x > n) then r := 1; end;
    return r;
  end A;
  public static method Two(f: () -> int, g: () -> int): int => f() + g();
  public static method B(): int
    var m: int := 7;
    return Two(lambda() => m, lambda() => m + 1);
  end B;
  public static method Main()
    var xs := new Array<int>();
    xs.Add(1); xs.Add(5);
    var i: int := 0;
    var tot: int := 0;
    while i < 200 do
      tot := tot + A(xs) + B();
      i := i + 1;
    end;
    Console.WriteLine(tot.ToString());
  end Main;
end Program;
