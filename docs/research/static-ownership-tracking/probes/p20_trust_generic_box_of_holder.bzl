module Probe;
// TRUST TODAY, generics: a generic owning box instantiated at a holder class. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: Box<Account> is borrow-bearing at this instantiation (an owned field whose substituted type is borrow-bearing), so `bx` carries `c`.
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
public class Box<T>
  private owned _v: T;
  public constructor(owned v: T)
    _v := v;
  end constructor;
  public method Get(): T => _v;
end Box;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var bx := new Box<Account>(new Account(c));
    c.Dispose();
    Console.WriteLine(bx.Get().OwnerName());
    return 0;
  end Main;
end Program;
