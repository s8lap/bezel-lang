//! case: s22_16_0017
//! doc: spec
//! section: 22.16
//! title: an override may add ensures clauses, conjoined with the inherited ones
//! rule: An override or implementation may add `ensures` clauses,
//!   conjoined with the inherited ones at its return edges —
//!   strengthening the postcondition, always sound for callers.
//! cites: 22.16
//! expect: accept
module Conf;

public class Base
  public virtual method Compute(x: int): int
    ensures(result >= 0)

    return x;
  end Compute;
end Base;

public class Derived: Base
  public override method Compute(x: int): int
    ensures(result <= 1000)

    return x;
  end Compute;
end Derived;
