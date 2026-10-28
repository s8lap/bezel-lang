module Conf;
// Family A: §28.3 "Property stores of a borrow-carrying value": `obj.P := param` is judged as the field store and admitted under the residual. EXPECT: report.
public class Customer
  public constructor()
  end constructor;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public property Owner: Customer
    get => _owner;
    set => _owner := value;
  end Owner;
end Account;
public class Program
  public static method Rebind(acct: Account, c: Customer)
    acct.Owner := c;
  end Rebind;
end Program;
