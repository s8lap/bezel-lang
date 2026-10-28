module Conf;
// §28.6 restatement: an owned field passed as a plain argument that the callee stores. The admission is in Account's constructor (reported there, a01); the caller side has no site. EXPECT: one report, in Account.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
end Account;
public class Bank
  private owned _c: Customer;
  private owned _a: Account;
  public constructor()
    _c := new Customer();
    _a := new Account(_c);
  end constructor;
end Bank;
