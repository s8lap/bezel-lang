class BankAccount
  private owned _transactions: OwningArray<Transaction>;

  public constructor()
    _transactions := new OwningArray<Transaction>();
  end constructor;
end BankAccount;
