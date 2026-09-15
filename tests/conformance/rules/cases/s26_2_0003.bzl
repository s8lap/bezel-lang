//! case: s26_2_0003
//! doc: spec
//! section: 26.2
//! title: a choice may not be extended
//! rule: The extendable list is closed against the choice kind: choices cannot be
//!   extended.
//! cites: 26.2, 35.2
//! expect: reject
//! diagnostic: extend
module Conf;

public choice Outcome
  Ok(value: int),
  Failed(message: string);
end Outcome;

extension Outcome
  public method IsOk2(): bool => true;
end;
