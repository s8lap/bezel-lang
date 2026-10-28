module Conf;
// Family A: field INITIALIZER reading a primary-constructor parameter (`private _o: Customer := owner;`). EXPECT: report.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account(owner: Customer)
  private _o: Customer := owner;
end Account;
