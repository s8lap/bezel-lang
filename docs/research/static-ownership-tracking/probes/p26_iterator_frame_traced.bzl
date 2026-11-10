module Probe;
// STATIC REGIME (frames): an iterator frame held in a local carries its plain argument; the argument is moved away while the frame is live. EXPECT (spec K6.4 + K5.5): compile error at the move. A frame is the existing precedent for "a class-typed value that carries a source set" (candidate A's holder).
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Program
  public static method Names(c: Customer): IEnumerable<string>
    yield return c.Name;
  end Names;
  public static method Take(owned c: Customer) => c.Dispose();
  public static method Main(): int
    var c := new Customer("ann");
    var names := Names(c);
    Take(move c);
    for var n in names do
      Console.WriteLine(n);
    end;
    return 0;
  end Main;
end Program;
