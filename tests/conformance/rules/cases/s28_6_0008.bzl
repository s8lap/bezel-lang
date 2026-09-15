//! case: s28_6_0008
//! doc: spec
//! section: 28.6
//! title: a moved owned field returned through a non-owned return escapes
//! rule: A `move`d owned field returned through a non-`owned` return type is
//!   an error — the moved value would escape with no owner.
//! cites: 28.6, 28.8.1
//! expect: reject
//! diagnostic: escape
module Conf;

public class Transaction
  public constructor()
  end constructor;
end Transaction;

public class BankAccount
  private owned _transactions: OwningArray<Transaction>;

  public constructor()
    _transactions := new OwningArray<Transaction>();
  end constructor;

  public method Leak(): OwningArray<Transaction>
    var fresh := new OwningArray<Transaction>();
    var taken := move _transactions;
    _transactions := fresh;
    return taken;
  end Leak;
end BankAccount;
