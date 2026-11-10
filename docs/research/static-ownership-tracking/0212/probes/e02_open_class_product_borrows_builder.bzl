module Probe;
// REFINEMENT COUNTER-EXAMPLE (critique round 2, note 2): the declared return type `Report` has one borrow field, a `Customer`, so a declaration-only test would say the result of `b.Build()` cannot hold a borrow of the `Builder`. `Report` is not sealed: `Build` returns a subclass that does. EXPECT today: compiles; checked = DisposedAccessException at `r.Show()`; unchecked = undefined. Under 0212 revision 2: the result carries `b`, so `Take(move b)` is frozen while `r` is live (K5.1 through K5.5). A refinement that reads only the declared type would accept this program.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Report
  private _c: Customer;
  public constructor(c: Customer)
    _c := c;
  end constructor;
  public virtual method Show(): string => _c.Name;
end Report;
public class Builder
  private _c: Customer;
  private _label: string;
  public constructor(c: Customer, label: string)
    _c := c;
    _label := label;
  end constructor;
  public property Label: string => _label;
  public method Build(): owned Report => new Spy(_c, this);
end Builder;
public class Spy: Report
  private _b: Builder;
  public constructor(c: Customer, b: Builder)
    base(c);
    _b := b;
  end constructor;
  public override method Show(): string => _b.Label;
end Spy;
public class Program
  public static method Take(owned b: Builder) => b.Dispose();
  public static method Main(): int
    using var c := new Customer("ann");
    var b := new Builder(c, "built by b");
    using var r := b.Build();
    Take(move b);
    Console.WriteLine(r.Show());
    return 0;
  end Main;
end Program;
