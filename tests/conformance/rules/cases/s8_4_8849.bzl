//! case: s8_4_8849
//! doc: spec
//! section: 8.4
//! title: `Object.ReferenceEquals` is a method reference at an explicit `(Object?, Object?) -> bool` target
//! rule: It is an ordinary static method with `Object?` parameters
//! cites: 8.1, 27.6, 27.1.1
//! expect: run
//! output: 1
//!  0
//!  1
//!  0
//! notes: Pins #9108 (fix da43840a9). The callee's own signature is `(Object?, Object?) -> bool`, so the reference is that function type. Arguments: (k,k) 1; (k,k2) 0;
//!  (null,null) 1; (k,null) 0 (the result is printed as 1 or 0). Red on the parent: the formed type had non-nullable parameters.
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
    var h: (Object?, Object?) -> bool := Object.ReferenceEquals;
    Console.WriteLine($"{(if h(k, k) then 1 else 0)}");
    Console.WriteLine($"{(if h(k, k2) then 1 else 0)}");
    Console.WriteLine($"{(if h(null, null) then 1 else 0)}");
    Console.WriteLine($"{(if h(k, null) then 1 else 0)}");
    return 0;
  end Main;
end Program;
