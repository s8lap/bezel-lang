//! case: s17_11_0057
//! doc: spec
//! section: 17.11
//! title: a non-defaultable class reaches default(T) through a three-link chain and is rejected
//! rule: For unconstrained `T`, `default(T)` is permitted; the types for
//!   which `default(T)` is defined are exactly the defaultable types
//!   (v0.338, §17.11).
//! cites: 17.11, 3.6
//! expect: reject
//! diagnostic: defaultable
//! issue: 5962
module Conf;
class W end W; record RS(A: string); enum E1 One := 1, Two := 2; end E1; static class UZ public static method Z<T>(): int var t := default(T); return 1; end Z; end UZ; static class C1 public static method F<S>(): int return UZ.Z<S>(); end F; end C1; static class C2 public static method F<S>(): int return C1.F<S>(); end F; end C2; static class C3 public static method F<S>(): int return C2.F<S>(); end F; end C3; class P public static method Main(): int return C3.F<string>(); end Main; end P;
