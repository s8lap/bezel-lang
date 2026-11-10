module Probe;
// ERASURE at a standard-library generic instantiated at an interface: an `owned Account` passed where `OwningArray<IShow>.Add` takes `owned T` at T = IShow (`Shared<IShow>` is not formable: r40 and stdlib 2.16 require a proper class type, so `Shared<T>` never erases to an interface). The conversion is at the CALLER (the parameter type after substitution is the interface), so the concrete rule decides it with no generic-body requirement. EXPECT today: compiles; faults. Under 0212: error at the conversion.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public interface IShow
  method Show(): string;
end IShow;
public class Account: IShow
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method Show(): string => _owner.Name;
end Account;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var xs := new OwningArray<IShow>();
    xs.Add(new Account(c));
    c.Dispose();
    Console.WriteLine(xs[0].Show());
    return 0;
  end Main;
end Program;
