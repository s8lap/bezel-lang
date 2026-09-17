//! case: s28_6_0030
//! doc: spec
//! section: 28.6
//! title: the same fresh owning value through an owned return transfers to the caller
//! rule: Handing a freshly-produced owning value to the caller is admissible
//!   through an `owned` return type — the caller assumes responsibility for
//!   its disposal — which is the remedy the non-`owned`-return rejection names.
//! cites: 28.6, 28.1, 28.8
//! expect: accept
//! issue: 4365
module Conf;

public class Transaction
  public constructor()
  end constructor;
end Transaction;

public class BankAccount
  public constructor()
  end constructor;

  public method MakeTransactions(): owned OwningArray<Transaction>
    return new OwningArray<Transaction>();
  end MakeTransactions;
end BankAccount;

public class Program
  public method Run(): int
    using var acct := new BankAccount();
    using var txs := acct.MakeTransactions();
    return txs.Count;
  end Run;
end Program;
