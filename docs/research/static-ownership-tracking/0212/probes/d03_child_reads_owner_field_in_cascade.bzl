module Probe;
// DESTRUCTOR WINDOW, k1a with a FIELD read: the child's destructor reads a public field of its owner (no accessor call) during the owner's cascade; the owner has no destructor body. MEASURE: fault in checked mode? output unchecked?
public class Owner
  public Label: string;
  private owned _child: Child? := null;
  public constructor(name: string)
    Label := name;
    _child := new Child(this);
  end constructor;
end Owner;
public class Child
  private _owner: Owner;
  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
  destructor
    Console.WriteLine($"child sees {_owner.Label}");
  end destructor;
end Child;
public class Program
  public static method Main(): int
    using var o := new Owner("Root");
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
