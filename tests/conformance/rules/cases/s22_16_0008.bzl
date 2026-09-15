//! case: s22_16_0008
//! doc: spec
//! section: 22.16
//! title: result is a compile error in a unit-typed member's clauses
//! rule: `result` is a compile error in a unit-typed member's clauses,
//!   in a constructor's clauses, and in any `requires` or `invariant`
//!   clause.
//! cites: 22.16
//! expect: reject
//! diagnostic: result
module Conf;

public class Ops
  public static method DoThing()
    ensures(result = 0)

  end DoThing;
end Ops;
