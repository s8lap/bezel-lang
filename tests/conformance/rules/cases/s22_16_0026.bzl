//! case: s22_16_0026
//! doc: spec
//! section: 22.16
//! title: clauses never distinguish overloads; two members differing only in clauses clash
//! rule: Clauses never distinguish overloads and are not part of §22.6
//!   resolution; two members differing only in clauses are the same
//!   signature and clash.
//! cites: 22.16, 22.6
//! expect: reject
//! diagnostic: same signature
module Conf;

public class Ops
  public static method Check(x: int): bool
    requires(x > 0)

    return true;
  end Check;

  public static method Check(x: int): bool
    requires(x < 100)

    return false;
  end Check;
end Ops;
