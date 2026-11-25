//! case: s27_1_1_8855
//! doc: spec
//! section: 27.1.1
//! title: a method reference returning `bool` is rejected at a `(Object?, Object?) -> int` target
//! rule: The reference's signature must be compatible with the target function type: its parameter and return types unify with the target's under the standard implicit-conversion rules
//! cites: 8.4, 27.6
//! expect: reject
//! diagnostic: convertible
//! notes: Pins #9108 (fix da43840a9). Per §27.6 (v0.359) the reference's return type `bool` must unify with the
//!   target's return type `int` under the standard implicit-conversion rules (§5.1); `bool` has no implicit
//!   conversion to `int` (§5.2 admits only the explicit `int(b)`), so the reference is not compatible with the
//!   target and is rejected. Green on both arms. Twin: 8849.
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
