//! case: s9_8_0038
//! doc: spec
//! section: 9.8
//! title: a record with a heap-collection component is not serialization-eligible
//! rule: A record with a class-, interface-, `Shared<T>`-, `Weak<T>`-, or
//!   heap-collection-typed serialized component is not serialization-eligible —
//!   a compile error at the conformance declaration, naming the field.
//! cites: 9.8, 3.1
//! expect: reject
//! diagnostic: serialization-eligible
//! notes: twin s9_8_0039 — the same component as an inline array is
//!   record-admissible.
module Conf;

public record Bag(Items: Array<int>): ISerializable, IDeserializable;
