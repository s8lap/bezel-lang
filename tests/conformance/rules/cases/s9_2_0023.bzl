//! case: s9_2_0023
//! doc: spec
//! section: 9.2
//! title: a record declaring a plain own-typed Equals and no GetHashCode is rejected
//! rule: On a value kind — a record, an enum or a choice — whose
//!   `Object`-slot `Equals` takes its own type and is declared `override` or
//!   plain, the two spellings declaring one member ... the rule reads both
//!   spellings of both members: a plain `Equals` obliges `GetHashCode`, and
//!   a plain `GetHashCode` discharges the obligation.
//! cites: 9.2, 8.3
//! expect: reject
//! diagnostic: GetHashCode
//! issue: 5924
module Conf;

public record Money
  public readonly property Amount: int;

  public constructor(amount: int)
    Amount := amount;
  end constructor;

  public method Equals(other: Money): bool => Amount mod 10 = other.Amount mod 10;
end Money;
