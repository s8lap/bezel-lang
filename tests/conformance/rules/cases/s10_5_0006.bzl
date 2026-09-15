//! case: s10_5_0006
//! doc: spec
//! section: 10.5
//! title: override is admitted on ToString
//! rule: An enum may declare — spelled `override` or plain — `Equals`
//!   taking the enum's own type, `GetHashCode(): int`, and `ToString(): string`;
//!   each replaces the compiler-generated default for every consumer.
//! cites: 10.5, 8.5
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public override method ToString(): string => "x";
end Priority;
