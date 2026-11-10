module Probe;
// TRUST TODAY, kind 2 in miniature: a method (not a constructor) re-points the holder's borrow at an inner-scope source; the holder is read after that scope ends. EXPECT (spec): compiles; one report line; DisposedAccessException in checked mode. Candidate A: the store adds `inner` to the holder's source set and is checked as an assignment to the holder's binding: "would outlive its source" (p02's rule).
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
  public method Reassign(owner: Customer) => _owner := owner;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    using var a := new Account(c);
    do
      using var inner := new Customer("bob");
      a.Reassign(inner);
    end;
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
