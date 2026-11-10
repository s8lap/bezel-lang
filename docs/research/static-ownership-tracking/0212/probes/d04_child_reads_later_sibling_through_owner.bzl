module Probe;
// DESTRUCTOR WINDOW, the genuinely dangling read: the child (declared FIRST, so disposed LAST) reads, through its owner, a sibling declared after it, which the cascade (reverse declaration order, §28.4) has already disposed. MEASURE: checked = fault; unchecked = ? (this is the read that is unsound even if the owner's block is live).
public class Leaf
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
  destructor
    Console.WriteLine("leaf disposed " + _name);
  end destructor;
end Leaf;
public class Owner
  private owned _child: Child? := null;
  private owned _late: Leaf;
  public constructor()
    _late := new Leaf("late");
    _child := new Child(this);
  end constructor;
  public method LateName(): string => _late.Name;
end Owner;
public class Child
  private _owner: Owner;
  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
  destructor
    Console.WriteLine($"child sees {_owner.LateName()}");
  end destructor;
end Child;
public class Program
  public static method Main(): int
    using var o := new Owner();
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
