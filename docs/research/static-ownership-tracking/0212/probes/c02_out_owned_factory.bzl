module Probe;
// FROM THE ADVERSARIAL CRITIQUE of the first draft (f72926c68), probe c02: an `out owned` recipient of a borrow-bearing class. 0212's mint covers only a declared `owned` RETURN; the out-owned recipient's set is unspecified. EXPECT today: compiles; checked = DisposedAccessException; unchecked = undefined. Under 0212 revision 2: an `out owned` recipient is a mint and carries the call's arguments, so `keep := move a` is the K2.3 error (finding 1).
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
public class Program
  public static method Make(c: Customer, out owned a: Account)
    a := new Account(c);
  end Make;
  public static method Main(): int
    var keep: Account? := null;
    do
      using var c := new Customer("ann");
      Make(c, out var a);
      keep := move a;
    end;
    Console.WriteLine(keep!.Show());
    return 0;
  end Main;
end Program;
