module Probe;
// GENERIC ERASURE, the note's hole: a generic body converts `owned T` to an `owned` interface. Instantiated at a class that holds a borrow, the result's type announces nothing and the body is not re-checked. EXPECT today: compiles; one report line (Account's store); checked = DisposedAccessException; unchecked = undefined. Under 0212: `Hider.Hide<Account>` is rejected where it is named (the conversion is a requirement of the body's use of T, read per instantiation).
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
end Hider;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var s := Hider.Hide<Account>(new Account(c));
    c.Dispose();
    Console.WriteLine(s.Show());
    return 0;
  end Main;
end Program;
