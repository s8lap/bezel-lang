module Probe;
// DESTRUCTOR WINDOW, the early-dispose idiom: the owner's destructor BODY disposes its back-pointing child explicitly (K9.3 admits disposing an owned field its class declares; the cascade's later call is the no-op). The child's destructor reads the owner while the owner's body is still running and every owned field is live. MEASURE: fault or clean, both modes.
public class Owner
  private _name: string;
  private owned _child: Child? := null;
  public constructor(name: string)
    _name := name;
    _child := new Child(this);
  end constructor;
  public property Name: string => _name;
  destructor
    Console.WriteLine("owner body begins");
    _child?.Dispose();
    Console.WriteLine("owner body ends");
  end destructor;
end Owner;
public class Child
  private _owner: Owner;
  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
  destructor
    Console.WriteLine($"child sees {_owner.Name}");
  end destructor;
end Child;
public class Program
  public static method Main(): int
    using var o := new Owner("Root");
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
