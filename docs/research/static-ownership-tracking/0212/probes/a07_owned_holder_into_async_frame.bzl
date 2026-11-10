module Probe;
// ASYNC, an OWNED holder transferred into the frame: the frame owns the `Account`, whose stored borrow is still the caller's `c`. EXPECT today: compiles; checked = DisposedAccessException when the caller disposes `c` before settlement. Under 0212: the owned argument brings its set to the task (K6 + the owned-argument rule), which is the frame-held clause: the report names the abandon, the verdict is unchanged.
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
  public static async method Keep(owned a: Account): Task<string>
    await Task.Delay(1);
    return a.OwnerName();
  end Keep;
  public static async method Main(): Task<int>
    var c := new Customer("ann");
    using var t := Keep(new Account(c));
    c.Dispose();
    Console.WriteLine(await t);
    return 0;
  end Main;
end Program;
