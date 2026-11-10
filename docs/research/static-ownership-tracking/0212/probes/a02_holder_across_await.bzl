module Probe;
// ASYNC, the question: a local instance that holds a borrow of a plain parameter is live across an `await` (the routine async helper). The frame already holds `c` itself until settlement, so the holder adds no source the frame does not carry. EXPECT today: compiles; one report line (Account store); clean when awaited. Under 0212: admitted — every source of a borrow formed in an async body is frame-resident or already carried by the frame (30.1).
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
    using var acct := new Account(c);
    await Task.Delay(1);
    return acct.OwnerName();
  end Work;
  public static async method Main(): Task<int>
    using var c := new Customer("ann");
    Console.WriteLine(await Work(c));
    return 0;
  end Main;
end Program;
