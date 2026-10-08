//! case: s10_5_0010
//! doc: spec
//! section: 10.5
//! title: a plain Equals taking Object? on an enum is rejected
//! rule: An `Equals` taking `Object?` or `Object` — the signature that fills
//!   a record's slot (§9.2) — is rejected on an enum at the declaration,
//!   spelled `override` or plain, the diagnostic naming the own-typed
//!   spelling
//! cites: 10.5, 8.1, 8.3
//! expect: reject
//! diagnostic: equals
//! issue: 5610
module Conf;

enum Priority
  Low, Medium, High;

  public method Equals(o: Object?): bool => false;
  public method GetHashCode(): int => int(this);
end Priority;
