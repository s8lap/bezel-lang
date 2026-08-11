class BankAccount
  private owned _mutex: Mutex := new Mutex();
  private _balance: decimal := 0m;

  public method Deposit(amount: decimal)
    using var guard := _mutex.Acquire();
    _balance := _balance + amount;
  end Deposit;
end BankAccount;
