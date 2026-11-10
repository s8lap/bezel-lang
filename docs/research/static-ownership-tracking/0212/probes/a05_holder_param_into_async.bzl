module Probe;
// ASYNC, a holder passed as a plain parameter: the frame holds a borrow of `acct`, and through it of `c`. EXPECT today: compiles; clean when awaited. Under 0212: the argument contributes its binding and the sources it carries to the task (K6), which is the frame-held clause unchanged; the awaited call is clean.
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
  public static async method Tell(a: Account): Task<string>
    await Task.Delay(1);
    return a.OwnerName();
  end Tell;
  public static async method Main(): Task<int>
    using var c := new Customer("ann");
    using var acct := new Account(c);
    Console.WriteLine(await Tell(acct));
    return 0;
  end Main;
end Program;
