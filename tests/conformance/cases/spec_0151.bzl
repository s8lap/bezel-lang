public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public method Deposit(amount: decimal)
    _balance := _balance + amount;
  end Deposit;
end BankAccount;
