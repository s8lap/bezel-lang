//! case: s26_4_0003
//! doc: spec
//! section: 26.4
//! title: a concrete instantiation is a legal extension head
//! rule: A concrete instantiation is written with keyword/primitive types,
//!   qualified names, or generic instantiations — none of which are bare
//!   single-segment identifiers, so none binds a parameter.
//! cites: 26.4
//! expect: accept
module Conf;

extension IEnumerable<int>
  public method HasAny(): bool => this.Any();
end;
