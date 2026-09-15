//! case: s13_7_0002
//! doc: spec
//! section: 13.7
//! title: a private field typed at a private nested class is admitted
//! rule: Every type appearing in a member's signature must be at least as
//!   visible as the member — `private _y: NestedPrivate;` is fine: also
//!   private, matches `NestedPrivate`'s visibility.
//! cites: 13.7, C.2
//! expect: accept
module Conf;

public class Outer
  private class NestedPrivate
    public constructor()
    end constructor;
  end NestedPrivate;

  public constructor()
  end constructor;

  private _x: NestedPrivate?;
end Outer;
