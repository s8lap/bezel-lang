//! case: s22_8_0001
//! doc: spec
//! section: 22.8
//! title: a field declared in a class may include an initializer
//! rule: A field declared in a class or record may include an initializer.
//! cites: 22.8
//! expect: accept
module Conf;

public record Transaction(Amount: decimal);

public class BankAccount
  private _balance: decimal := 0m;
  private owned _transactions: Array<Transaction> := new Array<Transaction>();
  private _createdAt: DateTime := DateTime.Now;

  public constructor()
  end constructor;
end BankAccount;

public class Runner
  public static method Run()
    using var acct := new BankAccount();
  end Run;
end Runner;
