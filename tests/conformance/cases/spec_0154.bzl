public class BankAccount(name: string, initialBalance: decimal)
  public readonly property Name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;

public class SavingsAccount(name: string, initialBalance: decimal, rate: decimal)
    : BankAccount(name, initialBalance)
  private _rate: decimal := rate;
end SavingsAccount;
