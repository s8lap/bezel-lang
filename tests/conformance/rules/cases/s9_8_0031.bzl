//! case: s9_8_0031
//! doc: spec
//! section: 9.8
//! title: a serialized clause on a record with no serialization conformance is rejected
//! rule: A `serialized` clause on a member of a type that declares no
//!   serialization conformance, or on a member of a type whose serialization is
//!   hand-written (synthesis suppressed — see below), is a compile error: the
//!   clause only has meaning as an input to synthesis,
//! cites: 9.8
//! expect: reject
//! diagnostic: serialized
//! notes: twin s9_8_0011 — the same rename on a conforming record.
module Conf;

public record Pair(A: int serialized "alpha", B: int);
