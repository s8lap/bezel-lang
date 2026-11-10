module Probe;
// DESTRUCTOR WINDOW, control: a back-pointing child with NO destructor (trivial-dispose). Nothing runs at its release, so nothing reads the owner in the cascade. EXPECT: clean in both modes.
public class Owner
  private _name: string;
  private owned _child: Child? := null;
  public constructor(name: string)
    _name := name;
    _child := new Child(this);
  end constructor;
  public property Name: string => _name;
  public method Ask(): string => _child!.OwnerName();
  destructor
    Console.WriteLine("owner body");
  end destructor;
end Owner;
public class Child
  private _owner: Owner;
  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Child;
public class Program
  public static method Main(): int
    using var o := new Owner("Root");
    Console.WriteLine(o.Ask());
    return 0;
  end Main;
end Program;
