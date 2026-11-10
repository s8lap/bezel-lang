module Probe;
// REACH OF scoped: the holder is a plain `var`, not a `using var`. EXPECT (spec §28.14: a bounding object that is no `using var` local admits nothing): compile error at the scoped argument. Candidate A tracks any holder binding, so this shape is admitted and bounded.
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
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    var a := new Account(scoped c);
    Console.WriteLine(a.OwnerName());
    a.Dispose();
    return 0;
  end Main;
end Program;
