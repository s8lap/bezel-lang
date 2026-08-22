public class BankAccount(name: string, initialBalance: decimal)
  public readonly property Name: string := name;
  private _balance: decimal := initialBalance;
end BankAccount;
