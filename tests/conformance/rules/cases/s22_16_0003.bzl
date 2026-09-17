//! case: s22_16_0003
//! doc: spec
//! section: 22.16
//! title: a contract clause's expression must be of type bool
//! rule: The expression must be of type `bool` (§4.1 — no truthiness).
//! cites: 22.16, 4.1
//! expect: reject
//! diagnostic: bool
module Conf;

public class Ops
  public static method Check(x: int)
    requires(x)

  end Check;
end Ops;
