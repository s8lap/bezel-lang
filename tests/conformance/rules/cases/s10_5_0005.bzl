//! case: s10_5_0005
//! doc: spec
//! section: 10.5
//! title: override is rejected on an enum member outside the Object-virtual surface
//! rule: `override` is admitted only on the `Object`-virtual members —
//!   `Equals`, `GetHashCode`, `ToString` — and is rejected on every other
//!   enum member.
//! cites: 10.5
//! expect: reject
//! diagnostic: override
module Conf;

enum Priority
  Low, Medium, High;

  public override method Describe(): string => "x";
end Priority;
