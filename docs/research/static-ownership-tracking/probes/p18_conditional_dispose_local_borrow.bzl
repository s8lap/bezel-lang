module Probe;
// CANDIDATE D: an explicit Dispose() on one path while a local borrow is live, then a use through the borrow. EXPECT (spec K4.2/K5.3: the announced kill is unfrozen; the use is not dominated, so 0205 does not reach it): compiles; DisposedAccessException in checked mode when the branch runs. A stricter freeze would reject the Dispose().
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Program
  public static method Main(args: Array<string>): int
    var c := new Customer("ann");
    var b := c;
    if args.Count = 0 then
      c.Dispose();
    end;
    Console.WriteLine(b.Name);
    return 0;
  end Main;
end Program;
