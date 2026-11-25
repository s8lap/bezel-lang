//! case: s27_1_1_8852
//! doc: spec
//! section: 27.1.1
//! title: a `(Object, Object) -> bool` value is rejected at a `(Object?, Object?) -> bool` target
//! rule: the target's parameter type must reach the value's by a reference conversion ([§5.1](#sec-implicit-conversions)), its nullable lift or the nullable view of a reference type, never the view's reverse
//! cites: 8.4, 27.6, 5.1
//! expect: reject
//! diagnostic: convertible
//! notes: Pins #9108 (fix da43840a9). Green on both arms (the parent also rejects it): a spec-decided boundary of the fix, which widened the Object slot relation but
//!  keeps the reverse refused. The value's parameters are non-nullable, the target's nullable: the view's reverse. Twin: 8851.
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
    var h: (Object, Object) -> bool := Object.ReferenceEquals;
    var g: (Object?, Object?) -> bool := h;
    Console.WriteLine($"{(if g(k, null) then 1 else 0)}");
    return 0;
  end Main;
end Program;
