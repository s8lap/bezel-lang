//! case: s9_8_0030
//! doc: spec
//! section: 9.8
//! title: serialized skip on a non-defaultable string component is rejected
//! rule: A `serialized skip` component whose type is not defaultable (§17.11) is
//!   a compile error at the conformance declaration (the synthesized
//!   `Deserialize` would have no value to construct with).
//! cites: 9.8, 17.11
//! expect: reject
//! diagnostic: defaultable
//! notes: twin s9_8_0048 (next free id; AUTHORING.md asks for adjacent ids, and renumbering
//!   the batch would churn every cross-reference).
module Conf;

public record Login(Id: int, Token: string serialized skip): ISerializable, IDeserializable;
