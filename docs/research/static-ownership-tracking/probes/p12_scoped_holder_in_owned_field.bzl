module Probe;
// REACH OF scoped: a two-level holder. Branch's constructor takes a scoped Customer and builds an Account over it, held in an owned field of the Branch. EXPECT (spec §28.14: a `scoped` borrow is stored only in a direct plain field of the bounding object or forwarded to a `scoped` parameter with the same bounding object; "storage reached through `owned` fields of the bounding object is excluded"): compile error. So the nested holder has no static spelling today; its trust form is p13. Candidate A admits it: Branch owns a borrow-bearing Account, so Branch is borrow-bearing and carries `c`.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(scoped owner: Customer)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Branch
  private owned _acct: Account;
  public constructor(scoped c: Customer)
    _acct := new Account(scoped c);
  end constructor;
  public method Who(): string => _acct.OwnerName();
end Branch;
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    using var b := new Branch(scoped c);
    Console.WriteLine(b.Who());
    return 0;
  end Main;
end Program;
