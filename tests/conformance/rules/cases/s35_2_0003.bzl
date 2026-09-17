//! case: s35_2_0003
//! doc: spec
//! section: 35.2
//! title: override on a non-Object-virtual choice member is rejected
//! rule: override is admitted exactly where §10.5 admits it — the Object-virtual
//!   surface, spelled override or plain — and is rejected on every other member.
//! expect: reject
//! diagnostic: override
module Conf;

public choice Outcome
  Ready,
  Empty;

  public override method Describe(): string => "ready";   // ERROR: Describe is not Equals/GetHashCode/ToString
end Outcome;
