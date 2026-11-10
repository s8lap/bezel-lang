module Probe;
// STATIC TODAY: a class with a plain class-typed field is not sendable (spec §30.14 rule 3), so a holder cannot be moved to another domain. EXPECT: compile error naming the property. Candidate A changes nothing here; "borrow-bearing" and "not transfer-closed" coincide on this rule.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public sealed class Account
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Program
  public static method Work(owned a: Account): int
    Console.WriteLine(a.OwnerName());
    return 0;
  end Work;
  public static method Main(): int
    using var c := new Customer("ann");
    var a := new Account(c);
    using var t := Domain.Spawn(Work, move a);
    return 0;
  end Main;
end Program;
