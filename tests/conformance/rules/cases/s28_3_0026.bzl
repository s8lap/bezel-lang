//! case: s28_3_0026
//! doc: spec
//! section: 28.3
//! title: the back-reference idiom admits a parameter-sourced borrow stored in a field
//! rule: A borrow whose source outlives the object is admissible: the back-reference
//!   idiom binds a constructor parameter into a non-owned class-typed field.
//! expect: accept
module Conf;

public class Customer
  public constructor()
  end constructor;
end Customer;

public class Account
  private _owner: Customer;

  public constructor(owner: Customer)
    _owner := owner;
  end constructor;

  public property Owner: Customer => _owner;
end Account;
