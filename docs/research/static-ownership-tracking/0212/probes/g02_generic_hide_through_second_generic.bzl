module Probe;
// GENERIC ERASURE through a second generic: `Outer<T>` only forwards its `owned T` to `Hider.Hide<T>`. EXPECT today: compiles; faults as g01. Under 0212: the requirement reaches `Outer` through the substitution (spec 3.6 rule 2), so `Outer<Account>` is rejected where it is named.
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
public class Hider
  public static method Hide<T>(owned x: T): owned IShow where T: IShow => x;
  public static method Outer<T>(owned x: T): owned IShow where T: IShow => Hide<T>(move x);
end Hider;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var s := Hider.Outer<Account>(new Account(c));
    c.Dispose();
    Console.WriteLine(s.Show());
    return 0;
  end Main;
end Program;
