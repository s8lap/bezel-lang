//! case: s28_6_0029
//! doc: spec
//! section: 28.6
//! title: a freshly-produced owning value through a non-owned return escapes
//! rule: A freshly-produced owning value (`return new OwningArray<T>(...)`)
//!   returned through a **non-`owned`** return type is an error: the value
//!   would escape with no owner and leak. Expose the factory as a method
//!   returning `owned T` instead.
//! cites: 28.6, 28.3
//! expect: reject
//! diagnostic: owner
//! issue: 4365
module Conf;

public class Transaction
  public constructor()
  end constructor;
end Transaction;

public class BankAccount
  public constructor()
  end constructor;

  public method LeakTransactions(): OwningArray<Transaction>
    return new OwningArray<Transaction>();
  end LeakTransactions;
end BankAccount;
