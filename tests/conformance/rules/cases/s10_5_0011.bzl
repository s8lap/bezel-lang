//! case: s10_5_0011
//! doc: spec
//! section: 10.5
//! title: an override Equals taking the enum's own type is admitted
//! rule: `override` is admitted only on the three `Object`-virtual members the
//!   next paragraph names, at exactly the signatures it names — `Equals`
//!   taking the enum's own type, `GetHashCode(): int`, `ToString(): string`
//! cites: 10.5, 8.3
//! expect: accept
module Conf;

enum Priority
  Low, Medium, High;

  public override method Equals(other: Priority): bool => int(this) = int(other);
  public override method GetHashCode(): int => int(this);
end Priority;
