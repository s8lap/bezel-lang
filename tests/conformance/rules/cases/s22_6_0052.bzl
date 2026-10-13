//! case: s22_6_0052
//! doc: spec
//! section: 22.6
//! title: a built-in exception operand is ambiguous between an Exception and an Object operator overload
//! rule: A call on an `Animal` argument's dog-typed operand versus
//!   `Object` is ambiguous when neither candidate is exact (v0.338,
//!   §22.6).
//! cites: 22.6, 5.1
//! expect: reject
//! diagnostic: ambiguous operator
//! issue: 6005
module Conf; record V(N: int) public static pure method operator +(a: V, e: Exception): V => new V(a.N + 1); public static pure method operator +(a: V, e: Object): V => new V(a.N + 9); end V; class P public static method Main(): int using var d := new ArgumentException("x"); var v := new V(0); Console.WriteLine((v + d).N); return 0; end Main; end P;
