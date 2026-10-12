//! case: s30_12_0003
//! doc: spec
//! section: 30.12
//! title: the Lock pattern with using var -- the guard's disposal is the release
//! rule: The `Lock` pattern with `using var`: `class BankAccount ... using
//!   var guard := _mutex.Acquire(); ... end Deposit; end BankAccount;` — the
//!   releasing of the lock is disposal of the `Lock`.
//! cites: 30.12
//! expect: run
//! stdout: 15
module Conf;
import System.Threading;

public class BankAccount
  private owned _mutex: Mutex := new Mutex();
  private _balance: decimal := 0m;

  public method Deposit(amount: decimal)
    using var guard := _mutex.Acquire();
    _balance := _balance + amount;
  end Deposit;

  public method Get(): decimal
    return _balance;
  end Get;
end BankAccount;

public class Program
  public static method Main()
    using var acct := new BankAccount();
    acct.Deposit(10m);
    acct.Deposit(5m);
    Console.WriteLine(acct.Get());
  end Main;
end Program;
