module Probe;
// NOT an erasure: a BORROW of a holder passed at an interface type to a constructor that stores it. Under 0212 the constructed `Keeper` carries what the argument contributes — the binding `a` and the sources `a` carries — read at the caller from the argument, not from the parameter type. EXPECT today: compiles; two report lines; checked = DisposedAccessException. Under 0212: the use after `c.Dispose()` is the dominated-use error (spec 28.2), through `k`.
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
public class Keeper
  private _s: IShow;
  public constructor(s: IShow)
    _s := s;
  end constructor;
  public method Tell(): string => _s.Show();
end Keeper;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var a := new Account(c);
    using var k := new Keeper(a);
    c.Dispose();
    Console.WriteLine(k.Tell());
    return 0;
  end Main;
end Program;
