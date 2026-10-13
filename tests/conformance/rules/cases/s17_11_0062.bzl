//! case: s17_11_0062
//! doc: spec
//! section: 17.11
//! title: a direct default of a type parameter named Array at string is rejected at the site
//! rule: For unconstrained `T`, `default(T)` is permitted; the types for
//!   which `default(T)` is defined are exactly the defaultable types
//!   (v0.338, §17.11).
//! cites: 17.11, 2.2
//! expect: reject
//! diagnostic: is not defaultable
//! issue: 6008
module Conf;
static class U6008 public static method Dflt<T>(): int var t := default(T); return 7; end Dflt; end U6008; static class Y6008 public static method ViaDflt<Array>(): int return U6008.Dflt<Array>(); end ViaDflt; end Y6008; static class D6008 public static method Direct<Array>(): int var a: Array := default(Array); return 5; end Direct; end D6008; class P public static method Main(): int return D6008.Direct<string>(); end Main; end P;
