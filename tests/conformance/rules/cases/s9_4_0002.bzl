//! case: s9_4_0002
//! doc: spec
//! section: 9.4
//! title: a bare store resolving to the receiver's own storage is rejected in a record method body
//! rule: this binds a borrow of the receiver value; a store that would land in
//!   the receiver's own storage is a compile error at the member body, including
//!   a bare X := e resolving to the receiver's storage.
//! expect: reject
//! diagnostic: borrow
module Conf;

public record Tally
  public property Hits: int;

  public constructor(hits: int)
    Hits := hits;
  end constructor;

  public method Bump()
    Hits := Hits + 1;
  end Bump;
end Tally;
