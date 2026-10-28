module Probe;
// Kind 3: a short-lived helper built over a local, used and disposed in the same scope; the source is disposed early while the helper lives. EXPECT (spec): compiles (the holder is invisible to K5/0205 across the call); one report line at the store; DisposedAccessException in checked mode; undefined behaviour unchecked. Derived retention (0132) would make `c.Dispose()` precede a dominated holder use: a compile error.
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
    using var a := new Account(c);
    c.Dispose();
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
