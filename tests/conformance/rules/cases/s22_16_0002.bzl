//! case: s22_16_0002
//! doc: spec
//! section: 22.16
//! title: a satisfied requires clause lets the member run normally
//! rule: A `requires` clause is evaluated on every invocation ...; a
//!   clause evaluating `false` raises `ContractViolationException`.
//!   (A clause evaluating `true` on every invocation lets the member
//!   proceed.)
//! cites: 22.16
//! expect: run
//! stdout: ok
module Conf;

public class Account
  private _balance: decimal := 0m;

  public constructor(balance: decimal)
    _balance := balance;
  end constructor;

  public method Withdraw(amount: decimal)
    requires(amount > 0m)
    requires(amount <= _balance)

    _balance := _balance - amount;
  end Withdraw;
end Account;

public class Program
  public static method Main()
    var a := new Account(10m);
    a.Withdraw(4m);
    Console.WriteLine("ok");
  end Main;
end Program;
