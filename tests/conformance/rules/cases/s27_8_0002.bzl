//! case: s27_8_0002
//! doc: spec
//! section: 27.8
//! title: a generic type alias may not reference itself
//! rule: A generic alias may not reference itself (no recursive aliases) and
//!   must resolve to a concrete type after substitution.
//! cites: 27.8
//! expect: reject
//! diagnostic: recursive
module Conf;

type Loop<T> := Loop<T>;
