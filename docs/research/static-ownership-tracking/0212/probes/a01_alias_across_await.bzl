module Probe;
// ASYNC baseline: a plain ALIAS of a plain parameter is live across an `await`; the caller awaits before the source dies. EXPECT (spec 30.1, borrowed parameters are held until settlement): compiles; no report line at this await; clean. Control for a02.
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
  public static async method Work(c: Customer): Task<string>
    var b := c;
    await Task.Delay(1);
    return b.Name;
  end Work;
  public static async method Main(): Task<int>
    using var c := new Customer("ann");
    Console.WriteLine(await Work(c));
    return 0;
  end Main;
end Program;
