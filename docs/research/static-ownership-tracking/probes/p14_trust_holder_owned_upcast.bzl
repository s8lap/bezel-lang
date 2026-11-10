module Probe;
// TRUST TODAY, the interface crux: a holder is returned OWNED at an interface type, so its static type announces no borrow. EXPECT (spec): compiles; one report line; DisposedAccessException in checked mode. Candidate A: the owned conversion of a borrow-bearing instance to a type that is not borrow-bearing is the borrow-erasing error (or the interface must be declared borrow-bearing).
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public interface IReport
  method Line(): string;
end IReport;
public class OwnerReport: IReport
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method Line(): string => _owner.Name;
end OwnerReport;
public class Reports
  public static method For(c: Customer): owned IReport => new OwnerReport(c);
end Reports;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var r := Reports.For(c);
    c.Dispose();
    Console.WriteLine(r.Line());
    return 0;
  end Main;
end Program;
