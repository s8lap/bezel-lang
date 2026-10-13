//! case: s17_11_0055
//! doc: spec
//! section: 17.11
//! title: an owned chain through a generic method reaches default(T) at string
//! rule: For unconstrained `T`, `default(T)` is permitted; the types for
//!   which `default(T)` is defined are exactly the defaultable types
//!   (v0.338, §17.11).
//! cites: 17.11, 3.6
//! expect: reject
//! diagnostic: defaultable
//! issue: 5962
module Conf;
static class U public static method Z<T>(): owned T return default(T); end Z; end U;
static class Y public static method Y1<S>(): owned S return U.Z<S>(); end Y1; end Y;
class P public static method Main(): int var s := Y.Y1<string>(); return 0; end Main; end P;
