//! case: s13_7_0003
//! doc: spec
//! section: 13.7
//! title: a protected member with an internal-typed parameter is rejected
//! rule: Every type appearing in a member's signature must be at least as
//!   visible as the member. The rule applies at every visibility level: a
//!   `protected` member's signature types must be at least `protected` —
//!   `internal` and `protected` are incomparable (§13.1), so an `internal`
//!   type does not satisfy it.
//! cites: 13.7, 13.1, C.2
//! expect: reject
//! diagnostic: as visible
module Conf;

public class Handler
  internal class Payload
    public constructor()
    end constructor;
  end Payload;

  public constructor()
  end constructor;

  protected method Accept(p: Payload) end Accept;
end Handler;
