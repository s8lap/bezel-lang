module Probe;
// CRITIQUE NOTE 7: `Shared<Account>.Clone()` at a borrow-bearing `Account`. The clone is an independent handle to the same value; read as an ordinary mint it would carry the ORIGINAL handle's binding, so moving the original away while the clone is used would be frozen — which defeats sharing. EXPECT today: compiles; clean in both modes (both handles are valid). Under 0212 revision 2: admitted — a clone carries the value's sources (`c`) and not the binding of the handle it was cloned from; the first draft would have rejected the `move first`.
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
  public static method Keep(owned s: Shared<Account>): owned Shared<Account> => s;
  public static method Main(): int
    using var c := new Customer("ann");
    var first := new Shared<Account>(new Account(c));
    using var second := first.Clone();
    using var kept := Keep(move first);
    Console.WriteLine(second.Get().Show());
    Console.WriteLine(kept.Get().Show());
    return 0;
  end Main;
end Program;
