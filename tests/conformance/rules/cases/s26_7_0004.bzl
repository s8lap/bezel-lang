//! case: s26_7_0004
//! doc: spec
//! section: 26.7
//! title: a value-kind extension body cannot store into the receiver's own
//!   storage
//! rule: For a value-kind receiver, §9.4's value-receiver rule applies in
//!   the extension body exactly as in a declared member body: a store
//!   landing in the receiver's own storage is a compile error.
//! cites: 26.7, 9.4
//! expect: reject
//! diagnostic: receiver
module Conf;

public record Point
  public property X: int;

  public constructor(x: int)
    X := x;
  end constructor;
end Point;

public extension Point
  public method Bump()
    X := X + 1;
  end Bump;
end;
