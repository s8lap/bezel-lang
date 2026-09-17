//! case: s22_16_0010
//! doc: spec
//! section: 22.16
//! title: result is a compile error in a constructor's clauses
//! rule: `result` is a compile error in a unit-typed member's clauses,
//!   in a constructor's clauses, and in any `requires` or `invariant`
//!   clause.
//! cites: 22.16
//! expect: reject
//! diagnostic: result
module Conf;

public class Widget
  private _n: int;

  public constructor(n: int)
    ensures(result = n)

    _n := n;
  end constructor;
end Widget;
