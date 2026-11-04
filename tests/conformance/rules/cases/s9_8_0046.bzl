//! case: s9_8_0046
//! doc: spec
//! section: 9.8
//! title: serialized skip on a choice case is rejected
//! rule: The `serialized` clause on a **case** admits the rename form only (a
//!   case is not omittable — the enum-constant rule);
//! cites: 9.8, 35.1
//! expect: reject
//! diagnostic: rename
//! notes: twin s9_8_0043 — a rename on a case is accepted.
module Conf;

public choice Mode: ISerializable, IDeserializable
  On serialized skip,
  Off;
end Mode;
