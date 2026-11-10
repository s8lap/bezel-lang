module Probe;
// TRUST TODAY: a holder of a local's borrow is moved into an object a static references (the static outlives every local). EXPECT (spec K4.3: "a borrow held inside an object a static references is that object's field-held clause"): compiles; DisposedAccessException in checked mode at the later read. Candidate A: `Vault` owns a borrow-bearing Account, so it is borrow-bearing; the store adds a non-static source to a value held by static storage: compile error at the call.
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
public class Vault
  private owned _last: Account? := null;
  public constructor()
  end constructor;
  public method Remember(owned a: Account) => _last := a;
  public method Last(): string => _last!.OwnerName();
end Vault;
public class App
  public static readonly Vault: Vault := new Vault();
end App;
public class Program
  public static method Fill()
    using var c := new Customer("ann");
    App.Vault.Remember(new Account(c));
  end Fill;
  public static method Main(): int
    Fill();
    Console.WriteLine(App.Vault.Last());
    return 0;
  end Main;
end Program;
