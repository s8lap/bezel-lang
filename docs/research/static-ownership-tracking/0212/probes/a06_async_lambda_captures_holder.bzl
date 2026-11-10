module Probe;
// ASYNC, a holder captured by an `async` lambda: the closure's set gains the holder's binding and the sources it carries; the task its invocation mints carries them. EXPECT today: compiles; clean when awaited. Under 0212: the same, with the capture counted (28.3 provenance clause 1).
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
  public static async method Main(): Task<int>
    using var c := new Customer("ann");
    using var acct := new Account(c);
    var tell := async lambda(): Task<string> => do
      await Task.Delay(1);
      return acct.OwnerName();
    end;
    Console.WriteLine(await tell());
    return 0;
  end Main;
end Program;
