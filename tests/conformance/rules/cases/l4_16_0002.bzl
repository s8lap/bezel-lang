//! case: l4_16_0002
//! doc: stdlib
//! section: 4.16
//! title: Slice is deliberately not a member of IReadOnlySpan<TElement>
//! rule: The interface deliberately omits Slice -- each concrete span type
//!   has its own Slice returning the same concrete type, and putting Slice
//!   on the interface would propagate the interface-typed handle.
//! cites: 4.16
//! expect: reject
//! diagnostic: Slice
module Conf;

public method Describe(s: IReadOnlySpan<int>)
  var head := s.Slice(0, 1);
end Describe;
