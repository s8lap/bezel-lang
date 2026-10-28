module Probe;
// Kind 3, the silent kill: the source is MOVED into a longer-lived owner (no flag set) while the helper still holds it, then the helper reads it — admitted today (the holder is not a local borrow, K5 cannot see it). Here the new owner disposes it before the read. EXPECT (spec): compiles; DisposedAccessException in checked mode. Under derived retention the `move c` is K5.1-frozen.
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
  public static method Sink(owned c: Customer)
    c.Dispose();
  end Sink;
  public static method Main(): int
    var c := new Customer("ann");
    using var a := new Account(c);
    Sink(move c);
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
