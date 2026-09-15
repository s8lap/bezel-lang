//! case: s22_16_0012
//! doc: spec
//! section: 22.16
//! title: old is a compile error in an invariant member
//! rule: `old` is a compile error in `requires` clauses, in `invariant`
//!   members, in constructor clauses, and outside clauses entirely.
//! cites: 22.16
//! expect: reject
//! diagnostic: old
module Conf;

public class Widget
  private _n: int := 0;

  invariant(old(_n) >= 0);

  public constructor()
  end constructor;
end Widget;
