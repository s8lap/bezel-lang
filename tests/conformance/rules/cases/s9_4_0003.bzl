//! case: s9_4_0003
//! doc: spec
//! section: 9.4
//! title: a compound assignment to receiver storage is rejected through its desugaring
//! rule: The rule keys on where the store would land; a compound assignment or
//!   ++/-- classifies through its section 6.6/17.6 desugaring as a store landing
//!   in the receiver's own storage.
//! cites: 6.6, 17.6
//! expect: reject
//! diagnostic: borrow
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Bump()
    Hits += 1;
  end Bump;
end Tally;
