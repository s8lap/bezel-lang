//! case: s11_1_0001
//! doc: spec
//! section: 11.1
//! title: a primary constructor chains to the base via inheritance-clause args
//! rule: If the class has a base class that requires constructor arguments,
//!   the base's arguments appear in parentheses after the base class name in
//!   the inheritance list; this desugars to a primary constructor that chains
//!   to base(...) before executing field initializers.
//! cites: 11.1
//! expect: accept
module Conf;

public class BankAccount(name: string, balance: decimal)
  private _name: string := name;
  private _balance: decimal := balance;

  public method Balance(): decimal => _balance;
end BankAccount;

public class SavingsAccount(name: string, balance: decimal, rate: decimal): BankAccount(name, balance)
  private _rate: decimal := rate;

  public method Rate(): decimal => _rate;
end SavingsAccount;
