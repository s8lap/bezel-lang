//! case: s22_6_0051
//! doc: spec
//! section: 22.6
//! title: a user-class operand is ambiguous between an Animal and an Object operator overload
//! rule: A call on an `Animal` argument's dog-typed operand versus
//!   `Object` is ambiguous when neither candidate is exact (v0.338,
//!   §22.6).
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: ambiguous operator
//! issue: 6005
module Conf; class Animal end Animal; class Dog: Animal end Dog; record V(N: int) public static pure method operator +(a: V, e: Animal): V => new V(a.N + 1); public static pure method operator +(a: V, e: Object): V => new V(a.N + 9); end V; class P public static method Main(): int using var d := new Dog(); var v := new V(0); Console.WriteLine((v + d).N); return 0; end Main; end P;
