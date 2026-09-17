//! case: s35_1_0007
//! doc: spec
//! section: 35.1
//! title: owned on a move-only-value payload field is admitted but redundant
//! rule: The owned payload marker is admitted-but-redundant on move-only value types
//!   (an owning inline array, or another owning choice).
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public choice AcquireOutcome
  Ready(owned conns: [4 owned]Connection?),   // redundant but admitted: the payload is move-only
  Empty;
end AcquireOutcome;
