//! case: s27_1_1_8855
//! doc: spec
//! section: 27.1.1
//! title: a method reference returning `bool` is rejected at a `(Object?, Object?) -> int` target
//! rule: **Value-typed, record-typed, and primitive slots are invariant.**
//! cites: 8.4, 27.6
//! expect: reject
//! diagnostic: convertible
//! notes: Pins #9108 (fix da43840a9). The return slot `bool` is a primitive slot and invariant: no representation-changing conversion applies. Green on both arms. Twin: 8849.
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
    var h: (Object?, Object?) -> int := Object.ReferenceEquals;
    Console.WriteLine($"{h(k, k)}");
    return 0;
  end Main;
end Program;
