module Probe;
// Kind 1: a child holds a back-pointer to the owner that holds it in an `owned` field. The owner's destructor body runs first (§28.4 step 1), then the owned fields cascade (step 2), so the child's destructor reads the owner AFTER the owner's body ran. EXPECT (spec): compiles; report names the store; run prints "owner body" then "child sees Root" (the owner's flag records that disposal began, checked mode faults from completion — measure).
public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(this);
  end constructor;
  public property Name: string => _name;
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
