module Probe;
// DESTRUCTOR WINDOW, the base-class route: destructors run subclass-to-base (§28.4), so the BASE destructor body runs after the derived class's owned fields are disposed. It calls a virtual member the derived class overrides, which reads a derived owned field. MEASURE: checked / unchecked. (Not a stored-borrow read: recorded to show the window exists without any back-pointer.)
public class Leaf
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Leaf;
public class Base
  public constructor()
  end constructor;
  public virtual method Describe(): string => "base";
  destructor
    Console.WriteLine("base body: " + Describe());
  end destructor;
end Base;
public class Derived: Base
  private owned _leaf: Leaf;
  public constructor()
    _leaf := new Leaf("leaf");
  end constructor;
  public override method Describe(): string => "derived " + _leaf.Name;
end Derived;
public class Program
  public static method Main(): int
    using var d := new Derived();
    Console.WriteLine("made");
    return 0;
  end Main;
end Program;
