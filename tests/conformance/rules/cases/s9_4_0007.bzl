//! case: s9_4_0007
//! doc: spec
//! section: 9.4
//! title: a this-rooted chain to receiver storage passed as an out argument is the same store by other syntax
//! rule: A this-rooted chain reaching the receiver's own storage passed as a
//!   ref/out argument is the same store by other syntax.
//! cites: 22.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public record Tally
  private _hits: int;

  public constructor(hits: int)
    _hits := hits;
  end constructor;

  public method Reset(out v: int)
    v := 0;
  end Reset;

  public method Rewire()
    Reset(out _hits);
  end Rewire;
end Tally;
