module Probe;
// TRUST TODAY: the two-level holder with plain parameters; the source is disposed while the outer holder lives. A per-parameter "retains" bit sees Account's constructor, but Branch's constructor stores no plain parameter of its own into a field: it stores an OWNED Account. EXPECT (spec): compiles; one report line (Account's store); DisposedAccessException in checked mode. Candidate A: Branch owns a borrow-bearing Account, so Branch is borrow-bearing and `b` carries `c`; the dominated use is a compile error.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Branch
  private owned _acct: Account;
  public constructor(c: Customer)
    _acct := new Account(c);
  end constructor;
  public method Who(): string => _acct.OwnerName();
end Branch;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var b := new Branch(c);
    c.Dispose();
    Console.WriteLine(b.Who());
    return 0;
  end Main;
end Program;
