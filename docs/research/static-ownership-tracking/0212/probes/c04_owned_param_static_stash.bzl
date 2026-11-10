module Probe;
// FROM THE ADVERSARIAL CRITIQUE of the first draft (f72926c68), probe c04: an owned borrow-bearing instance passed to a STATIC method (no receiver) that hands it into another parameter's object. 0212 adds an owned argument's sources "to the constructed object or the receiver": here there is neither. EXPECT today: compiles; checked = DisposedAccessException; unchecked = undefined. Under 0212 revision 2: an `owned` parameter of a borrow-bearing class carries an opaque caller source in the callee, so `b.Put(move a)` — a store into an object the body does not hold — is an error in `Give` (finding 3).
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
  public method Show(): string => _owner.Name;
end Account;
public class Box
  private owned _a: Account? := null;
  public constructor()
  end constructor;
  public method Put(owned a: Account)
    _a := a;
  end Put;
  public method Show(): string => _a!.Show();
end Box;
public class Program
  public static method Give(owned a: Account, b: Box)
    b.Put(move a);
  end Give;
  public static method Main(): int
    using var box := new Box();
    do
      using var c := new Customer("ann");
      Give(new Account(c), box);
    end;
    Console.WriteLine(box.Show());
    return 0;
  end Main;
end Program;
