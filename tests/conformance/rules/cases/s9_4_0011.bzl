//! case: s9_4_0011
//! doc: spec
//! section: 9.4
//! title: an explicit this-rooted store into a record's own property is rejected in a method
//! rule: Consequently a store that would land in the **receiver's own storage**
//!   is a compile error at the member body: a borrow admits no store, and no
//!   receiver-position reading of such a store is sound. The receiver's own
//!   storage is what an lvalue chain rooted at `this` reaches without leaving
//!   the value — an explicit `this.X := e`, …
//! expect: reject
//! diagnostic: receiver
//! notes: s9_4_0002 pins the bare `X := e` spelling; this is the explicit one.
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Reset()
    this.Hits := 0;
  end Reset;
end Tally;
