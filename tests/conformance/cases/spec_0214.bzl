class BankAccount
  private owned _transactions: Array<Transaction>;

  public constructor()
    _transactions := new Array<Transaction>();
  end constructor;

  destructor
    _transactions.DisposeElements();
  end destructor;
end BankAccount;
