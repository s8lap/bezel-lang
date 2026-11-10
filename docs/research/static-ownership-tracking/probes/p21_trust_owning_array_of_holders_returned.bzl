module Probe;
// TRUST TODAY: holders collected in an owning collection and returned; the source is a local of the caller that is transferred away while the collection lives (a silent kill no freeze sees). EXPECT (spec): compiles; DisposedAccessException in checked mode. Candidate A: OwningArray<Account> is borrow-bearing at this instantiation and the returned value carries the call's plain argument `c`: the move is frozen (K5.1).
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
public class Bank
  public static method OpenTwo(c: Customer): owned OwningArray<Account>
    var xs := new OwningArray<Account>();
    xs.Add(new Account(c));
    xs.Add(new Account(c));
    return xs;
  end OpenTwo;
  public static method Close(owned c: Customer) => c.Dispose();
end Bank;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var accts := Bank.OpenTwo(c);
    Bank.Close(move c);
    Console.WriteLine(accts[0].OwnerName());
    return 0;
  end Main;
end Program;
