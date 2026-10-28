module Conf;
// §28.14 scoped construction twin. EXPECT: quiet.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(scoped owner: Customer)
    _owner := owner;
  end constructor;
end Account;
