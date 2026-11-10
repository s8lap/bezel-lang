module Probe;
// FROM THE RE-CRITIQUE of revision 2 (4dfa0e331), probe e01: a builder whose owned `Build()` result borrows the builder's ARGUMENT, never the builder. Under 0212 revision 2 the result carries the local `b` (the taken-out rule), so `return b.Build();` is rejected: a scope-bound local source. MEASURE: what r40 says today. The clean rewrite keeps the builder out of the result's life: `return new Report(c);`, or a `Build` the caller of `Make` calls on a builder it holds.
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
  public method Show(): string => _c.Name;
end Report;
public class Builder
  private _c: Customer;
  public constructor(c: Customer)
    _c := c;
  end constructor;
  public method Build(): owned Report => new Report(_c);
end Builder;
public class Program
  public static method Make(c: Customer): owned Report
    var b := new Builder(c);
    return b.Build();
  end Make;
  public static method Main(): int
    using var c := new Customer("ann");
    using var r := Make(c);
    Console.WriteLine(r.Show());
    return 0;
  end Main;
end Program;
