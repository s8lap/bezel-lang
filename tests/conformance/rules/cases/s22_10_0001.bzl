//! case: s22_10_0001
//! doc: spec
//! section: 22.10
//! title: primary constructor parameters are in scope inside field initializers
//! rule: Primary-constructor parameters are in scope inside field
//!   initializers.
//! cites: 22.10
//! expect: accept
module Conf;

public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public method Balance(): decimal => _balance;
end BankAccount;
