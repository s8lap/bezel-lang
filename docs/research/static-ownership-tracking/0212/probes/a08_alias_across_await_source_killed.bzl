module Probe;
// ASYNC, the control for a03: a plain ALIAS of a plain parameter is live across an `await`, and the caller disposes the source while the task is unsettled. EXPECT (spec 30.1, the frame-held clause): compiles; checked = DisposedAccessException. The same residual as a03, with no holder in the frame.
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
    var c := new Customer("ann");
    using var t := Work(c);
    c.Dispose();
    Console.WriteLine(await t);
    return 0;
  end Main;
end Program;
