//! case: s11_2_0001
//! doc: spec
//! section: 11.2
//! title: a class implements an interface implicitly by declaring matching public members
//! rule: A class implements an interface implicitly: by declaring matching
//!   public members.
//! cites: 11.2
//! expect: accept
module Conf;

public interface IBankAccount
  method Deposit(amount: decimal);
  readonly property AccountNumber: string;
end IBankAccount;

public class BankAccount: IBankAccount
  private _balance: decimal := 0m;

  public readonly property AccountNumber: string;

  public constructor(number: string)
    AccountNumber := number;
  end constructor;

  public method Deposit(amount: decimal)
    _balance := _balance + amount;
  end Deposit;
end BankAccount;
