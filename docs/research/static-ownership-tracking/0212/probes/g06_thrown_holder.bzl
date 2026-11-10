module Probe;
// ERASURE at `throw`: an exception that holds a borrow of a scope-bound local is thrown past that local's scope and caught at a type that announces nothing. EXPECT today: compiles; one report line; checked = DisposedAccessException in the handler. Under 0212: error at the `throw` (the operand's source set is not empty and a catch site re-establishes none).
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
public class CtxError: Exception
  private _who: Customer;
  public constructor(who: Customer) base("ctx");
    _who := who;
  end constructor;
  public method Who(): string => _who.Name;
end CtxError;
public class Program
  public static method Work()
    using var c := new Customer("ann");
    throw new CtxError(c);
  end Work;
  public static method Main(): int
    try
      Work();
    catch e: CtxError =>
      Console.WriteLine(e.Who());
    end;
    return 0;
  end Main;
end Program;
