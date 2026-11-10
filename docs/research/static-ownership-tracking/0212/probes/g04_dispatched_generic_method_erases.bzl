module Probe;
// GENERIC ERASURE in a DISPATCHED generic method: an interface method with its own type parameter has no instantiation site for the bodies dispatch reaches (spec 14.2). The implementing body stores `owned T` at an interface type. EXPECT today: compiles; faults. Under 0212: an error at the implementing body unless the constraint set establishes that T holds no borrows (`where T: sendable` does).
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
public interface ISink
  method Take<T>(owned x: T) where T: IShow;
  method Last(): string;
end ISink;
public class Sink: ISink
  private owned _kept: IShow? := null;
  public constructor()
  end constructor;
  public method Take<T>(owned x: T) where T: IShow
    _kept := x;
  end Take;
  public method Last(): string
    var k := _kept;
    if k <> null then
      return k.Show();
    end;
    return "none";
  end Last;
end Sink;
public class Program
  public static method Main(): int
    using var sink := new Sink();
    var s: ISink := sink;
    var c := new Customer("ann");
    s.Take<Account>(new Account(c));
    c.Dispose();
    Console.WriteLine(s.Last());
    return 0;
  end Main;
end Program;
