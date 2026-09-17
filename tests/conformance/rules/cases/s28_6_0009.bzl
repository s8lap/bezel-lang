//! case: s28_6_0009
//! doc: spec
//! section: 28.6
//! title: a moved owned field is returned through an owned return via take-restore-return
//! rule: Transferring an owned field's value out to the caller is only
//!   admissible through an `owned` return type, in the take-restore-return
//!   shape: build the replacement, take the field, restore it, then return
//!   the taken value.
//! cites: 28.6, 28.8.1
//! expect: accept
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

  public method Take(): owned OwningArray<Transaction>
    var fresh := new OwningArray<Transaction>();
    var taken := move _transactions;
    _transactions := fresh;
    return taken;
  end Take;
end BankAccount;
