//! case: s22_10_0015
//! doc: spec
//! section: 22.10
//! title: a primary constructor's inheritance-clause base call replaces an explicit base(...)
//! rule: When a class with a primary constructor has a base class, the
//!   base call appears in the inheritance clause. This replaces an
//!   explicit `base(...)` call.
//! cites: 22.10
//! expect: run
//! stdout: 100
module Conf;

public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public method Balance(): decimal => _balance;
end BankAccount;

public class SavingsAccount(name: string, initialBalance: decimal, rate: decimal)
    : BankAccount(name, initialBalance)
  private _rate: decimal := rate;
end SavingsAccount;

public class Program
  public static method Main()
    var s := new SavingsAccount("Al", 100m, 2m);
    Console.WriteLine(int(s.Balance()));
  end Main;
end Program;
