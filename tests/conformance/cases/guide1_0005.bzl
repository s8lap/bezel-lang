class BankAccount(owner: Customer)
  private owned _transactions: Array<Transaction> := new Array<Transaction>();
  private owned _cache: TransactionCache? := null;   // nullable owned
  private _owner: Customer := owner;                  // back-reference, NOT owned
end BankAccount;
