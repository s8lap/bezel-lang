//! case: s17_9_0002
//! doc: spec
//! section: 17.9
//! title: nameof of a dotted expression returns the rightmost identifier
//! rule: "For a dotted expression, the result is the rightmost identifier."
//!   Example: `nameof(BankAccount.Deposit) // "Deposit"`.
//! cites: 17.9
//! expect: run
//! stdout: Deposit
module Conf;

public class BankAccount
  public constructor()
  end constructor;

  public method Deposit(amount: decimal)
  end Deposit;
end BankAccount;

public class Program
  public static method Main()
    Console.WriteLine(nameof(BankAccount.Deposit));
  end Main;
end Program;
