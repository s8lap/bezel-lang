//! case: s8_4_8850
//! doc: spec
//! section: 8.4
//! title: `Object.ReferenceEquals` and `K.ReferenceEquals` are admitted as function-typed arguments
//! rule: It is an ordinary static method with `Object?` parameters
//! cites: 8.1, 11.7, 22.12, 27.6
//! expect: run
//! output: 1
//!  0
//! notes: Pins #9108 (fix da43840a9). The class-qualified spelling `K.ReferenceEquals` (the inherited static) and `Object.ReferenceEquals` passed to `Apply`.
//!  (k,k) 1; (k,null) 0. Red on the parent (both spellings were rejected as arguments).
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
    Console.WriteLine($"{(if Program.Apply(K.ReferenceEquals, k, k) then 1 else 0)}");
    Console.WriteLine($"{(if Program.Apply(Object.ReferenceEquals, k, null) then 1 else 0)}");
    return 0;
  end Main;
end Program;
