module Probe;
// REACH OF scoped: a static factory forwards its scoped parameter into the constructor and returns the holder owned; the caller binds it to a `using var` (bounding form 2). EXPECT (spec §28.14): compiles; no report line; static bound.
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
public class Bank
  public static method Open(scoped c: Customer): owned Account => new Account(scoped c);
end Bank;
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    using var a := Bank.Open(scoped c);
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
