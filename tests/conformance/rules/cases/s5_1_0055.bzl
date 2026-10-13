//! case: s5_1_0055
//! doc: spec
//! section: 5.1
//! title: a lone Object operator overload is applicable to a class operand by reference conversion
//! rule: A reference conversion of a reference-typed value is
//!   representation-preserving, the same reference viewed at a wider
//!   static type (v0.338, §5.1).
//! cites: 5.1, 25
//! expect: run
//! output: 9
//! issue: 6005
module Conf; class Animal end Animal; class Dog: Animal end Dog; record V(N: int) public static pure method operator +(a: V, e: Object): V => new V(a.N + 9); end V; class P public static method Main(): int using var d := new Dog(); var v := new V(0); Console.WriteLine((v + d).N); return 0; end Main; end P;
