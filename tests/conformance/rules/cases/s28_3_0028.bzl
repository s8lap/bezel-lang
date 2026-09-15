//! case: s28_3_0028
//! doc: spec
//! section: 28.3
//! title: a borrow of an owned parameter stored in a field is rejected
//! rule: An `owned` parameter is scope-bound: the method disposes its undischarged
//!   value at its own end, so a borrow of it is rejected in a field store exactly
//!   as a borrow of a scope-bound local is.
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Customer
  public constructor()
  end constructor;
end Customer;

public class Account
  private _owner: Customer;

  public constructor(owned c: Customer)
    _owner := c;
  end constructor;
end Account;
