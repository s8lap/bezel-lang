//! case: s9_8_0033
//! doc: spec
//! section: 9.8
//! title: an empty serialized rename is rejected
//! rule: The string must be a non-empty literal and must be unique among the
//!   **serialized names** of the type's components
//! cites: 9.8
//! expect: reject
//! diagnostic: non-empty
//! notes: twin s9_8_0011 — a non-empty rename is accepted.
module Conf;

public record Pair(A: int serialized "", B: int): ISerializable, IDeserializable;
