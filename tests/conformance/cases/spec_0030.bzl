public class SavingsAccount(name: string, balance: decimal, rate: decimal): BankAccount(name, balance)
  private _rate: decimal := rate;
end SavingsAccount;
