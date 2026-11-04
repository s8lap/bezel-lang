//! case: s9_8_0037
//! doc: spec
//! section: 9.8
//! title: serialized skip on an enum constant is rejected
//! rule: Not admitted on enum constants (Appendix A grammar note): an enum
//!   value is a single member, not an omittable field.
//! cites: 9.8, 10.1
//! expect: reject
//! diagnostic: skip
//! notes: twin s9_8_0008 — a rename on the same constant is accepted.
module Conf;

public enum Level: ISerializable, IDeserializable
  Low serialized skip, High;
end Level;
