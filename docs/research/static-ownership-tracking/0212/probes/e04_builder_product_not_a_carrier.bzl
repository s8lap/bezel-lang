module Probe;
// CRITIQUE ROUND 2, NOTE 2: probe e01 with a product that is NOT of a carrier type — `Build` returns a fresh `Customer`, which stores no borrow. MEASURE: r40 rejects `return b.Build();` here too, with the same message, so that verdict does not depend on the product. Under 0212: a value that is not of a carrier type carries nothing, and the line is admitted.
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
  public method Build(): owned Customer => new Customer("fresh");
end Builder;
public class Program
  public static method Make(c: Customer): owned Customer
    var b := new Builder(c);
    return b.Build();
  end Make;
  public static method Main(): int
    using var c := new Customer("ann");
    using var r := Make(c);
    Console.WriteLine(r.Name);
    return 0;
  end Main;
end Program;
