//! case: s9_8_0008
//! doc: spec
//! section: 9.8
//! title: an enum is always serialization-eligible and admits a constant rename
//! rule: Enums are always serialization-eligible. The synthesized form writes the
//!   member's serialized name as a string — its `serialized "<name>"` rename
//!   where one is declared, its identifier otherwise.
//! cites: 10.5
//! expect: accept
module Conf;

public enum Level: ISerializable, IDeserializable
  Low serialized "low", High;
end Level;
