//! case: s2_7_8401
//! doc: spec
//! section: 2.7
//! title: a derived class's name followed by the inherited static ReferenceEquals is a method
//!   reference
//! rule: If `A` resolves to a *type*, the rest of the chain resolves against that type's
//!   members, declared or inherited
//! cites: 2.7, 8.1, 27.6
//! expect: run
//! output: False
//!   True
//!   False
//!   True
//!   True
//!   False
//! notes: Pins #9028 (the READ form through a class type name). `D.ReferenceEquals` names
//!   `Object`'s one static (`ReferenceEquals(a: Object?, b: Object?)`, §8.1), which `D`
//!   inherits; `f` is a bound-free reference to it. `f(d, d2)` compares two distinct `D`
//!   instances (False), `f(d, d)` (True), `f(k, d)` a `K` and a `D` (False), `g(k, k)`
//!   through `Object.ReferenceEquals` (True), `f(n, n)` null with null (True), `f(k, n)`
//!   (False).
module Conf;
import System;
public class K end K;
public class D : K end D;
public class Program
  public static method Main(): int
    var k := new K();
    var d := new D();
    var d2 := new D();
    var f := D.ReferenceEquals;
    Console.WriteLine(f(d, d2));
    Console.WriteLine(f(d, d));
    Console.WriteLine(f(k, d));
    var g := Object.ReferenceEquals;
    Console.WriteLine(g(k, k));
    var n: Object? := null;
    Console.WriteLine(f(n, n));
    Console.WriteLine(f(k, n));
    return 0;
  end Main;
end Program;
