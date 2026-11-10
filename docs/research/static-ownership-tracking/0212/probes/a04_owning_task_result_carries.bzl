module Probe;
// ASYNC, a result that holds a borrow: an `async` method returns an `OwningTask<Account>` built over its plain parameter. The awaited result is an owned instance whose stored borrow is the caller's `c`. EXPECT today: compiles; checked = DisposedAccessException after `c.Dispose()`. Under 0212: the task carries `c` (K6), the awaited result carries the task's carried set, so the use after the dominating `c.Dispose()` is the 28.2 error.
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
  public static async method Open(c: Customer): OwningTask<Account>
    await Task.Delay(1);
    return new Account(c);
  end Open;
  public static async method Main(): Task<int>
    var c := new Customer("ann");
    using var acct := await Open(c);
    c.Dispose();
    Console.WriteLine(acct.OwnerName());
    return 0;
  end Main;
end Program;
