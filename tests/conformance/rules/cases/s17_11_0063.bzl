//! case: s17_11_0063
//! doc: spec
//! section: 17.11
//! title: default of a non-shadowing built-in Array type is still rejected
//! rule: For unconstrained `T`, `default(T)` is permitted; the types for
//!   which `default(T)` is defined are exactly the defaultable types
//!   (v0.338, §17.11).
//! cites: 17.11
//! expect: reject
//! diagnostic: is a compile error for a non-nullable reference type
//! issue: 6008
module Conf;
class P public static method Main(): int var a: Array<int> := default(Array<int>); return 0; end Main; end P;
