module Conf;
// Control: a scope-bound local stored into a field is REJECTED (no report, an error). EXPECT: error.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account
  private _owner: Customer;
  public constructor()
    using var c := new Customer();
    _owner := c;
  end constructor;
end Account;
