module Probe;
// CANDIDATES A/B/D: the same conditional kill, with the borrow held by a class holder. EXPECT (spec): compiles; DisposedAccessException in checked mode when the branch runs. Under A or B the holder is a traced borrow, and the conditional announced kill stays the flag's (K4.2) unless the stricter freeze (D) is also taken.
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
  public static method Main(args: Array<string>): int
    var c := new Customer("ann");
    using var a := new Account(c);
    if args.Count = 0 then
      c.Dispose();
    end;
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
