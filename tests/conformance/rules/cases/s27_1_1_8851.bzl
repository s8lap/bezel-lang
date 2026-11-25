//! case: s27_1_1_8851
//! doc: spec
//! section: 27.1.1
//! title: a `(Object?, Object?) -> bool` value is usable at a `(Object, Object) -> bool` target
//! rule: **Parameter slots** of class or interface type, nullable or not, are **contravariant**
//! cites: 8.4, 27.6
//! expect: run
//! output: 1
//! notes: Pins #9108 (fix da43840a9). The reference is formed at `(Object?, Object?) -> bool` and then retyped to `(Object, Object) -> bool`: the target's parameter
//!  `Object` reaches the value's `Object?` by the nullable lift. g(k,k) prints 1. Accepting twin of 8852.
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
    var g: (Object, Object) -> bool := h;
    Console.WriteLine($"{(if g(k, k) then 1 else 0)}");
    return 0;
  end Main;
end Program;
