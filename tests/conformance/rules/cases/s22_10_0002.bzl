//! case: s22_10_0002
//! doc: spec
//! section: 22.10
//! title: primary constructor parameters are in scope in the inheritance clause's base call
//! rule: Primary-constructor parameters are in scope inside the
//!   inheritance clause's base call; this replaces an explicit `base(...)`
//!   call.
//! cites: 22.10
//! expect: accept
module Conf;

public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;

public class SavingsAccount(name: string, initialBalance: decimal, rate: decimal)
    : BankAccount(name, initialBalance)
  private _rate: decimal := rate;
end SavingsAccount;
