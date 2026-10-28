module Probe;
// Kind 1, the subtlety: the child is moved OUT of its owner (a slot kill by move, §28.8.1), then the owner is disposed, then the child's back-pointer is read. The owner-outlives-owned proof does not cover a moved-out child. EXPECT (spec): compiles; checked mode raises DisposedAccessException at `c.OwnerName()`.
public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(this);
  end constructor;
  public property Name: string => _name;
  public method Detach(): owned Child => move _child!;
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
    var o := new Owner("Root");
    using var c := o.Detach();
    o.Dispose();
    Console.WriteLine(c.OwnerName());
    return 0;
  end Main;
end Program;
