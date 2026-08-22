class BankAccount
  private owned _transactions: Array<Transaction>;       // non-nullable, owned
  private owned _cache: TransactionCache?;               // nullable, owned
  private _owner: Customer;                              // back-reference, not owned
end BankAccount;
