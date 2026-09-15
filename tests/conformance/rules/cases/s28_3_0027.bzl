//! case: s28_3_0027
//! doc: spec
//! section: 28.3
//! title: a borrow of a scope-bound local stored in a field is rejected
//! rule: A borrowed scope-bound local never satisfies the field-store rule: it is
//!   disposed at the end of its scope, which a field of the object outlives.
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Customer
  public constructor()
  end constructor;
end Customer;

public class Account
  private _owner: Customer;

  public constructor()
    var c := new Customer();
    _owner := c;
  end constructor;
end Account;
