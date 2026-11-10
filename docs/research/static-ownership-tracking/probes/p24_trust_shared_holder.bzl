module Probe;
// TRUST TODAY: a holder moved into a Shared<T> handle; the source is disposed while the handle lives. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: Shared<Account> is borrow-bearing at this instantiation; the handle and every Clone() carry `c`.
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
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var s := new Shared<Account>(new Account(c));
    using var t := s.Clone();
    c.Dispose();
    Console.WriteLine(t.Get().OwnerName());
    return 0;
  end Main;
end Program;
