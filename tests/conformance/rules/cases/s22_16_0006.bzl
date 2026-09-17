//! case: s22_16_0006
//! doc: spec
//! section: 22.16
//! title: throw is rejected in a contract clause
//! rule: Three clause-specific amendments: `throw` is rejected (a clause
//!   states a proposition; the mechanism, message, and blame of a
//!   violation belong to this section's machinery).
//! cites: 22.16, 22.15
//! expect: reject
//! diagnostic: throw
module Conf;

public class Ops
  public static method Check(x: int): bool
    requires(throw new ArgumentException("boom"))

    return true;
  end Check;
end Ops;
