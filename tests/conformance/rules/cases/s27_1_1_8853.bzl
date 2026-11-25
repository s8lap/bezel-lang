//! case: s27_1_1_8853
//! doc: spec
//! section: 27.1.1
//! title: a method reference is rejected at a function type with a different number of parameter slots
//! rule: a value is never usable at a function type with a different number of parameter slots, whatever the slots' types
//! cites: 8.4, 27.6
//! expect: reject
//! diagnostic: convertible
//! notes: Pins #9108 (fix da43840a9). `Object.ReferenceEquals` has two parameters; the target `(Object?) -> bool` has one. Green on both arms. Twin: 8849.
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
    var h: (Object?) -> bool := Object.ReferenceEquals;
    Console.WriteLine($"{(if h(k) then 1 else 0)}");
    return 0;
  end Main;
end Program;
