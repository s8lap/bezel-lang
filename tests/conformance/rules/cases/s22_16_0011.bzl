//! case: s22_16_0011
//! doc: spec
//! section: 22.16
//! title: old is a compile error in a requires clause
//! rule: `old` is a compile error in `requires` clauses, in `invariant`
//!   members, in constructor clauses, and outside clauses entirely.
//! cites: 22.16
//! expect: reject
//! diagnostic: old
module Conf;

public class Ops
  public static method Check(x: int): bool
    requires(old(x) >= 0)

    return true;
  end Check;
end Ops;
