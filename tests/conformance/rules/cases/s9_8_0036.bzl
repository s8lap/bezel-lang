//! case: s9_8_0036
//! doc: spec
//! section: 9.8
//! title: the serialized-name uniqueness rule is not relaxed for same-valued enum constants
//! rule: The rule is not relaxed for same-valued constants (§10.2): the write
//!   direction uses the first-declared name for a shared value
//!   (*Synthesis-eligibility*'s duplicate-constant rule), so a second
//!   constant's duplicate rename could never be written —
//! cites: 9.8, 10.2
//! expect: reject
//! diagnostic: unique
//! notes: twin s9_8_0017 — the same aliases without the colliding rename.
module Conf;

public enum Code: ISerializable, IDeserializable
  Legacy := 1, Current := 1 serialized "Legacy", Next := 2;
end Code;
