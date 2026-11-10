module Probe;
// PRECISION OF A (constructor arguments): the holder's constructor takes two class-typed arguments and keeps only the first; the second is disposed while the holder lives. EXPECT (spec): compiles; runs clean (no stale read exists). Under A's mint rule the holder carries BOTH arguments. As written the program is still accepted: the kill is an announced Dispose() (K4.2, unfrozen) and §28.2's dominated-use error needs EVERY source in the set disposed. The same shape with a silent kill of `o` (a transfer or a reassignment: K5.1, K5.2), or with `o` declared in an inner scope (K2.3), is rejected although nothing dangles: the over-approximation's cost. A-explicit (the stored parameter marked `scoped`) carries only `c`.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Options
  private _verbose: bool;
  public constructor(verbose: bool)
    _verbose := verbose;
  end constructor;
  public property Verbose: bool => _verbose;
end Options;
public class Account
  private _owner: Customer;
  private _loud: bool;
  public constructor(owner: Customer, o: Options)
    _owner := owner;
    _loud := o.Verbose;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Account;
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    var o := new Options(true);
    using var a := new Account(c, o);
    o.Dispose();
    Console.WriteLine(a.OwnerName());
    return 0;
  end Main;
end Program;
