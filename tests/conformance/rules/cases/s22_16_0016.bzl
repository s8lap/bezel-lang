//! case: s22_16_0016
//! doc: spec
//! section: 22.16
//! title: an override may not declare requires clauses
//! rule: An override or implementation may not declare `requires`
//!   clauses. Strengthening a precondition breaks every caller
//!   dispatching through the base contract.
//! cites: 22.16
//! expect: reject
//! diagnostic: requires
module Conf;

public class Base
  public virtual method Withdraw(amount: decimal)
    requires(amount > 0m)

  end Withdraw;
end Base;

public class Derived: Base
  public override method Withdraw(amount: decimal)
    requires(amount < 1000m)

  end Withdraw;
end Derived;
