//! case: s35_1_0005
//! doc: spec
//! section: 35.1
//! title: a payload transitively containing the choice's own type via a record is rejected
//! rule: A choice payload field may not, directly or transitively through value-typed
//!   components (record fields, inline-array elements, other choices' payloads),
//!   have the declaring choice's own type.
//! expect: reject
//! diagnostic: contain itself
module Conf;

public record Box(Item: Loop);

public choice Loop
  Base,
  Wrapped(item: Box);   // ERROR: Box.Item is Loop — transitively self-containing
end Loop;
