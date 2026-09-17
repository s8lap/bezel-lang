//! case: s9_4_0007
//! doc: spec
//! section: 9.4
//! title: a this-rooted chain to receiver storage passed as an out argument is the same store by other syntax
//! rule: A this-rooted chain reaching the receiver's own storage passed as a
//!   ref/out argument is the same store by other syntax; an out argument may be
//!   a property with an accessible setter.
//! cites: 17.6, 22.3
//! expect: reject
//! diagnostic: borrow
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Reset(out v: int)
    v := 0;
  end Reset;

  public method Rewire()
    Reset(out Hits);
  end Rewire;
end Tally;
