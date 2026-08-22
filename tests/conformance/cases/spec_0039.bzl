public class BankAccount(name: string, initialBalance: decimal): IBankAccount
  private _name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;

public class SavingsAccount(name: string, balance: decimal, rate: decimal): BankAccount(name, balance)
  private _rate: decimal := rate;
end SavingsAccount;
