//! case: s9_8_0007
//! doc: spec
//! section: 9.8
//! title: a serialized rename colliding with another component's name is rejected
//! rule: The string must be a non-empty literal and must be unique among the
//!   serialized names of the type's components; a member's serialized name is
//!   its rename where one is declared and its declared identifier otherwise, so
//!   a rename equal to another member's identifier collides too. A collision is
//!   a compile error at the conformance declaration.
//! expect: reject
//! diagnostic: unique
module Conf;

public record Pair(A: int serialized "B", B: int): ISerializable, IDeserializable;
