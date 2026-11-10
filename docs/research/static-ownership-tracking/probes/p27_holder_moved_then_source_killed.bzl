module Probe;
// TRUST TODAY, the moved holder: the holder is moved into another binding (and could be passed on), then the source is transferred away. A caller-side rule keyed on "the holder is a `using var`" (0132's v1 scope, §28.14's bounding forms) does not follow the move. EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: the source set travels with the value; the transfer of `c` is frozen while `b` is live (K5.1).
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
  public method OwnerName(): string => _owner.Name;
end Account;
public class Program
  public static method Close(owned c: Customer) => c.Dispose();
  public static method Main(): int
    var c := new Customer("ann");
    var a := new Account(c);
    var b := move a;
    Close(move c);
    Console.WriteLine(b.OwnerName());
    b.Dispose();
    return 0;
  end Main;
end Program;
