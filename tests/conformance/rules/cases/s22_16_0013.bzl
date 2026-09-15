//! case: s22_16_0013
//! doc: spec
//! section: 22.16
//! title: old is a compile error in a constructor's clauses
//! rule: `old` is a compile error in `requires` clauses, in `invariant`
//!   members, in constructor clauses, and outside clauses entirely
//!   (fields sit at §22.7's null watermark — no prestate exists).
//! cites: 22.16, 22.7
//! expect: reject
//! diagnostic: old
module Conf;

public class Widget
  private _n: int;

  public constructor(n: int)
    ensures(old(_n) = 0)

    _n := n;
  end constructor;
end Widget;
