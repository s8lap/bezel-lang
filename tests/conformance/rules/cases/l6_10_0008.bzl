//! case: l6_10_0008
//! doc: stdlib
//! section: 6.10
//! title: an owned TaskScope field is rejected
//! rule: Every other owning position is a compile error naming this rule, at
//!   the declaration that spells the position or at the expression that would
//!   place a `TaskScope` in one, `TaskScope?` reading as `TaskScope`
//!   throughout: ... any position language spec §28.1 admits `owned` at — a
//!   parameter (`out` and lambda parameters included), a return slot, a
//!   field, a choice payload, a function-type slot, an owning inline-array
//!   slot.
//! cites: 6.10, 28.1
//! expect: reject
//! diagnostic: await using
//! issue: 5657
module Conf;

import System.Threading;

public class Holder
  private owned _scope: TaskScope? := null;

  public constructor()
  end constructor;
end Holder;
