//! case: l2_14_0002
//! doc: stdlib
//! section: 2.14
//! title: a serialized rename beginning with $ is a compile error
//! rule: Field names beginning with $ are reserved for the identity mapping
//!   ($id, $ref, $type, $case); a serialized "<name>" rename whose name
//!   begins with $ is a compile error at the conformance declaration.
//! cites: 2.14, 2.15
//! expect: reject
//! diagnostic: $
module Conf;

public record Bad(X: int serialized "$id"): ISerializable, IDeserializable;
