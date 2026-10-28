module Conf;
// Run probe for the #4602 free-at-Dispose amendment: a field-held trust admission; the caller disposes the source early (K4.2) while the holder lives; a later read through the holder must be the K4.1 deterministic fault, not a freed read. EXPECT (spec): DisposedAccessException in the default build.
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
