module Probe;
// FROM THE ADVERSARIAL CRITIQUE of the first draft (f72926c68), probe c05: an owning choice whose payload is an `owned` borrow-bearing class. 0212's source set is defined for "an instance of a borrow-bearing class"; the choice value is not one, and a static field of the choice type is not an R5 owning position. EXPECT today: compiles; checked = DisposedAccessException; unchecked = undefined. Under 0212 revision 2: an owning choice with an `owned` payload of a borrow-bearing class is a carrier, `Slot.Full(...)` is a mint, and `Wrap` returns a value that carries `c`, so `s := Wrap(c)` is the K2.3 error (finding 2).
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
public choice Slot
  Full(owned a: Account),
  Empty;
end Slot;
public class Program
  public static method Wrap(c: Customer): Slot => Slot.Full(new Account(c));
  public static method Main(): int
    var s: Slot := Slot.Empty;
    do
      using var c := new Customer("ann");
      s := Wrap(c);
    end;
    var msg := (case s of
      Full(a) => a.Show(),
      Empty => "none");
    Console.WriteLine(msg);
    return 0;
  end Main;
end Program;
