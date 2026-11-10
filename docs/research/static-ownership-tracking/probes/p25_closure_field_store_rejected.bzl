module Probe;
// STATIC REGIME (function values): a capture-bearing closure stored in a field through a plain parameter. EXPECT (spec §28.14 call-bounded default): compile error in the callee — a plain function-typed parameter is never stored. This is the posture candidate A1 gives a plain CLASS-typed parameter.
public class Customer
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Customer;
public class Button
  private _onClick: (() -> ())?;
  public constructor()
  end constructor;
  public method Bind(f: () -> ()) => _onClick := f;
end Button;
public class Program
  public static method Main(): int
    using var c := new Customer("ann");
    using var b := new Button();
    b.Bind(lambda() => Console.WriteLine(c.Name));
    return 0;
  end Main;
end Program;
