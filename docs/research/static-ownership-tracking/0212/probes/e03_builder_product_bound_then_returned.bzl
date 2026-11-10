module Probe;
// CRITIQUE ROUND 2, NOTE 2: probe e01 with the product BOUND to a local before it is returned. MEASURE: r40 accepts this form and it runs clean, so its rejection of `return b.Build();` (e01) is a rule about a directly returned call on a local, not about what the product holds. Under 0212 revision 2: `r` carries the local `b` (the taken-out rule), so `return r;` is rejected — a clean rejection, the over-approximation §4.7 records.
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
    var r := b.Build();
    return r;
  end Make;
  public static method Main(): int
    using var c := new Customer("ann");
    using var r := Make(c);
    Console.WriteLine(r.Show());
    return 0;
  end Main;
end Program;
