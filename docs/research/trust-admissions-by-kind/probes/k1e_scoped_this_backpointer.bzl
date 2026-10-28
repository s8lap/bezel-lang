module Probe;
// Kind 1 spelled with today's static form: `new Child(scoped this)` under §28.14's `this` arm (form 3: the result is assigned to an owned field of this). EXPECT (spec): compiles; no report line (a scoped construction records no trust clause).
public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(scoped this);
  end constructor;
  public property Name: string => _name;
  public method ChildOwner(): string => _child!.OwnerName();
end Owner;
public class Child
  private _owner: Owner;
  public constructor(scoped owner: Owner)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Child;
public class Program
  public static method Main(): int
    using var o := new Owner("Root");
    Console.WriteLine(o.ChildOwner());
    return 0;
  end Main;
end Program;
