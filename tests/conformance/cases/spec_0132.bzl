class BankAccount
  private _balance: decimal := 0m;
  private owned _transactions: Array<Transaction> := new Array<Transaction>();
  private _createdAt: DateTime := DateTime.Now;
end BankAccount;
