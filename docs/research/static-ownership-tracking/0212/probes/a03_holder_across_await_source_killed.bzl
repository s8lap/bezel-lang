module Probe;
// ASYNC, the residual that stays: the caller disposes the source while the task is unsettled, then awaits. The holder inside the frame reads a disposed object. This is the frame-held clause of 30.1 (settlement), which 0212 does not retire — the same fault occurs with the plain alias of a01. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: unchanged (frame-held clause; a task is not traced by K5, K6.4).
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
    var c := new Customer("ann");
    using var t := Work(c);
    c.Dispose();
    Console.WriteLine(await t);
    return 0;
  end Main;
end Program;
