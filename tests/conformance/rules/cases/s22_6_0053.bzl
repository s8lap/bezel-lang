//! case: s22_6_0053
//! doc: spec
//! section: 22.6
//! title: an exact operator overload beats an Object overload on a class operand
//! rule: An exact match is the only relation that distinguishes one
//!   applicable candidate from another at an argument position (v0.338,
//!   §22.6).
//! cites: 22.6
//! expect: run
//! output: 1
//! issue: 6005
module Conf; class Animal end Animal; class Dog: Animal end Dog; record V(N: int) public static pure method operator +(a: V, e: Dog): V => new V(a.N + 1); public static pure method operator +(a: V, e: Object): V => new V(a.N + 9); end V; class P public static method Main(): int using var d := new Dog(); var v := new V(0); Console.WriteLine((v + d).N); return 0; end Main; end P;
