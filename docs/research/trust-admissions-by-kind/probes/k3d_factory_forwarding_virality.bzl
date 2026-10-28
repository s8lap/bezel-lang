module Probe;
// Virality: the holder is built by a factory that forwards its plain parameter (`Open(c)` -> `new Account(c)`), and the factory is called through a second forwarding helper. A `kept`-style marker (or `scoped`) would be needed on Account's ctor, Open's and Wire's parameters: three signatures for one store. EXPECT (spec): compiles; one report line (the store); the early dispose faults in checked mode.
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
public class Bank
  public static method Open(c: Customer): owned Account => new Account(c);
  public static method Wire(c: Customer): owned Account => Open(c);
end Bank;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var a := Bank.Wire(c);
    c.Dispose();
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
