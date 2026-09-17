//! case: s35_2_0002
//! doc: spec
//! section: 35.2
//! title: a virtual method on a choice is rejected
//! rule: Choice members are statically dispatched; virtual and abstract are rejected
//!   as on enums and records.
//! expect: reject
//! diagnostic: virtual
module Conf;

public choice Outcome
  Ready,
  Empty;

  public virtual method Describe(): string => "ready";   // ERROR: no `virtual` on a choice member
end Outcome;
