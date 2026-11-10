module Probe;
// FROM THE ADVERSARIAL CRITIQUE of the first draft (f72926c68), probe c01: rule 2 of 0212 §3.1 fires only where the substituted value type is a borrow-bearing CLASS. Here T := IStore, a borrow-bearing INTERFACE (a scoped store member); the owned value is erased to IShow (not borrow-bearing). EXPECT today: compiles; checked = DisposedAccessException; unchecked = undefined. Under 0212 revision 2: `Hide<IStore>` is rejected where it is named — the per-instantiation requirement reads a borrow-bearing class OR INTERFACE (finding 5).
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
public interface IStore: IShow
  method Attach(scoped c: Customer);
end IStore;
public class Account: IStore
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method Attach(scoped c: Customer)
    _owner := c;
  end Attach;
  public method Show(): string => _owner.Name;
end Account;
public class Hider
  public static method Hide<T>(owned x: T): owned IShow where T: IShow => x;
end Hider;
public class Program
  public static method Up(owned a: Account): owned IStore => a;
  public static method Main(): int
    var c := new Customer("ann");
    var st := Up(new Account(c));
    using var s := Hider.Hide<IStore>(move st);
    c.Dispose();
    Console.WriteLine(s.Show());
    return 0;
  end Main;
end Program;
