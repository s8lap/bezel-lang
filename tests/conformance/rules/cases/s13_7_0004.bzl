//! case: s13_7_0004
//! doc: spec
//! section: 13.7
//! title: a protected member with a protected-typed parameter is admitted
//! rule: Every type appearing in a member's signature must be at least as
//!   visible as the member; equal visibility satisfies the rule.
//! cites: 13.7, C.2
//! expect: accept
module Conf;

public class Handler
  protected class Payload
    public constructor()
    end constructor;
  end Payload;

  public constructor()
  end constructor;

  protected method Accept(p: Payload) end Accept;
end Handler;
