module Conf;
// Family A: a primary-constructor parameter read from a body captures it into a compiler-generated field (§22.10) — a field-held borrow. EXPECT: report.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account(owner: Customer)
  public method Who(): Customer => owner;
end Account;
