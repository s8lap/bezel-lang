//! case: s9_8_0035
//! doc: spec
//! section: 9.8
//! title: an enum constant renamed to another constant's identifier is rejected
//! rule: The string must be a non-empty literal and must be unique among the
//!   **serialized names** of the type's components — … — and, alike, unique
//!   among an enum's constants, and among a choice's cases; a member's
//!   serialized name is its rename where one is declared and its declared
//!   identifier otherwise, so a rename equal to another member's identifier
//!   collides too.
//! cites: 9.8, 10.1
//! expect: reject
//! diagnostic: unique
//! notes: twin s9_8_0008 — a non-colliding constant rename is accepted.
module Conf;

public enum Level: ISerializable, IDeserializable
  Low serialized "High", High;
end Level;
