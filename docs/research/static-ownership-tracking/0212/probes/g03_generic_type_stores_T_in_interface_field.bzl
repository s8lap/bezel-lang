module Probe;
// GENERIC ERASURE in a generic TYPE: the constructor stores its `owned T` in an `owned` field declared at an interface. EXPECT today: compiles; faults. Under 0212: a requirement of the constructed type, so `Erase<Account>` is rejected wherever it is named (as `Keep<Pair>` is today).
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
public class Erase<T> where T: IShow
  private owned _v: IShow;
  public constructor(owned v: T)
    _v := v;
  end constructor;
  public method Show(): string => _v.Show();
end Erase;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var e := new Erase<Account>(new Account(c));
    c.Dispose();
    Console.WriteLine(e.Show());
    return 0;
  end Main;
end Program;
