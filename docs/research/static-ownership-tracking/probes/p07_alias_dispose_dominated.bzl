module Probe;
// BASELINE (0205): a local alias used after a dominating explicit disposal of its source. EXPECT (spec §28.2, v0.355): compile error at the use. Control for p05 (the same rule through a record holder).
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    var b := c;
    c.Dispose();
    Console.WriteLine(b.Name);
    return 0;
  end Main;
end Program;
