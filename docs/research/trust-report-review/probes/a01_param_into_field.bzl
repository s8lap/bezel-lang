module Conf;
// Family A, §28.3 field-store clause 3 (s28_3_0090 shape): plain ctor parameter into a plain field. EXPECT: report.
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
