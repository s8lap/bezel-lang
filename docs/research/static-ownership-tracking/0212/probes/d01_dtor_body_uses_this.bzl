module Probe;
// DESTRUCTOR WINDOW, flag timing: the owner's own destructor body (a) reads an expression-bodied property of `this`, (b) calls an instance method of `this`, (c) hands `this` to a static helper that reads a property. MEASURE which of the three faults in checked mode: it tells whether the disposed flag is set before the destructor body runs, and whether a member entry on `this` inside the body is flag-tested.
public class Owner
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  public method Describe(): string => "owner " + _name;
  destructor
    Console.WriteLine("field: " + _name);
    Console.WriteLine("property: " + Name);
    Console.WriteLine("method: " + Describe());
    Console.WriteLine("helper: " + Program.Peek(this));
  end destructor;
end Owner;
public class Program
  public static method Peek(o: Owner): string => o.Name;
  public static method Main(): int
    using var o := new Owner("Root");
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
