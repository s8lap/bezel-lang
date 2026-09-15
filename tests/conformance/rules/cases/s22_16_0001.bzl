//! case: s22_16_0001
//! doc: spec
//! section: 22.16
//! title: a violated requires clause raises ContractViolationException in every build mode
//! rule: A `requires` clause is evaluated on every invocation, after
//!   argument binding and before any execution of the member; a clause
//!   evaluating `false` raises `ContractViolationException`, in every
//!   build mode.
//! cites: 22.16, 31.4
//! expect: run
//! stdout: caught
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
    try
      a.Withdraw(-5m);
    catch
      var ex: ContractViolationException => Console.WriteLine("caught"),
    end;
  end Main;
end Program;
