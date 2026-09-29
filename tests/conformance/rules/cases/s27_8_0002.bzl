//! case: s27_8_0002
//! doc: spec
//! section: 27.8
//! title: a generic type alias may not reference itself
//! rule: An alias may not reference itself, directly or through other
//!   aliases, in its aliased type or in its `where` clause.
//! cites: 27.8
//! expect: reject
//! diagnostic: recursive
module Conf;

type Loop<T> := Loop<T>;
