//! case: s26_2_0002
//! doc: spec
//! section: 26.2
//! title: a nullable type may not be extended directly
//! rule: Nullable types may not be extended directly (`extension int?`); extend the
//!   non-nullable form and let callers handle nullability at the call site.
//! cites: 26.2, 7.5
//! expect: reject
//! diagnostic: nullable
module Conf;

extension int?
  public property IsPositive: bool => true;
end;
