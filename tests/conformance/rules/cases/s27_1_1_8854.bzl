//! case: s27_1_1_8854
//! doc: spec
//! section: 27.1.1
//! title: a method reference over `Object?` slots is rejected at a primitive `(int, int) -> bool` target
//! rule: **Value-typed, record-typed, and primitive slots are invariant.**
//! cites: 8.4, 27.6
//! expect: reject
//! diagnostic: convertible
//! notes: Pins #9108 (fix da43840a9). A primitive `int` slot is invariant: boxing is not a reference conversion and no adapter exists. Green on both arms. Twin: 8849.
module Conf;
import System;
public class K end K;
public class Program
  public static method Apply(f: (Object?, Object?) -> bool, a: Object?, b: Object?): bool
    return f(a, b);
  end Apply;
  public static method Main(): int
    var k := new K();
    var k2 := new K();
    var h: (int, int) -> bool := Object.ReferenceEquals;
    Console.WriteLine($"{(if h(1, 2) then 1 else 0)}");
    return 0;
  end Main;
end Program;
