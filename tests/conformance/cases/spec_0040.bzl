public class BankAccount(name: string, initialBalance: decimal)
  private _name: string := name;
  private _balance: decimal := initialBalance;

  public constructor(name: string)
    this(name, 0m);
  end constructor;
end BankAccount;
