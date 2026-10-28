module Probe;
// Kind 3 at its narrowest: an unbound temporary passed to a storing constructor (FCG §4.3 corollary 5; DECLINED #5360). The temporary dies at the statement's end (K8.1); the holder dangles. EXPECT (spec): compiles; "customer disposed" printed before the read; DisposedAccessException in checked mode at `a.OwnerName()`. (The review's e06 found r35 does not dispose the temporary, #6775 — measure.)
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  destructor
    Console.WriteLine("customer disposed");
  end destructor;
end Customer;
public class Account
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Program
  public static method Main(): int
    using var a := new Account(new Customer("tmp"));
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
