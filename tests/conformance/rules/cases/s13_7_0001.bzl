//! case: s13_7_0001
//! doc: spec
//! section: 13.7
//! title: an internal field typed at a private nested class is rejected
//! rule: Every type appearing in a member's signature must be at least as
//!   visible as the member — `internal _x: NestedPrivate;` is an error when
//!   `NestedPrivate` is `private`: the field's type is not as visible as the
//!   field.
//! cites: 13.7, C.2
//! expect: reject
//! diagnostic: as visible
module Conf;

public class Outer
  private class NestedPrivate
    public constructor()
    end constructor;
  end NestedPrivate;

  public constructor()
  end constructor;

  internal _x: NestedPrivate?;
end Outer;
