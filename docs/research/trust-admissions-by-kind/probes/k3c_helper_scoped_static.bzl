module Probe;
// Kind 3 spelled statically with `scoped`: the source is frozen while the holder lives, so the early `c.Dispose()` is REJECTED at compile time (§28.14: the freeze covers explicit disposal). EXPECT (spec): compile error at `c.Dispose()`; no report line.
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
    using var a := new Account(scoped c);
    c.Dispose();
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
