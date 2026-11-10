module Probe;
// NOT an erasure: a generic box whose every owning position is declared at T. `Box<Account>` holds borrows at this instantiation (an owned field whose substituted type does), so `bx` carries `c` and no requirement arises. EXPECT today: compiles; checked = DisposedAccessException. Under 0212: admitted as a type; the use after `c.Dispose()` is the dominated-use error. The value taken out is returned `owned` from an instance member, so it carries the receiver and with it `c`. (Control for g01-g04; the prior note's p20. The bare take at a nullable field is used because r40 does not implement the replacing `move`.)
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public interface IShow
  method Show(): string;
end IShow;
public class Account: IShow
  private _owner: Customer;
  public constructor(owner: Customer)
    _owner := owner;
  end constructor;
  public method Show(): string => _owner.Name;
end Account;
public class Box<T>
  private owned _v: T?;
  public constructor(owned v: T)
    _v := v;
  end constructor;
  public method Take(): owned T? => move _v;
end Box;
public class Program
  public static method Main(): int
    var c := new Customer("ann");
    using var bx := new Box<Account>(new Account(c));
    using var old := bx.Take();
    c.Dispose();
    if old <> null then
      Console.WriteLine(old.Show());
    end;
    return 0;
  end Main;
end Program;
