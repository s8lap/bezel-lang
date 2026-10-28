module Probe;
// Kind 1 over an owning collection: children in an `owned OwningArray<Child>` each hold `this`; one is popped out (ownership leaves the owner), the owner is disposed, the popped child reads its back-pointer. EXPECT (spec): compiles; DisposedAccessException in checked mode at the second read; the un-popped child is disposed in the owner's cascade.
public class Owner
  private _name: string;
  private owned _kids: OwningArray<Child> := new OwningArray<Child>();
  public constructor(name: string)
    _name := name;
    _kids.Add(new Child(this));
    _kids.Add(new Child(this));
  end constructor;
  public property Name: string => _name;
  public method PopOne(): owned Child => _kids.PopLast();
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
    using var c := o.PopOne();
    Console.WriteLine(c.OwnerName());
    o.Dispose();
    Console.WriteLine(c.OwnerName());
    return 0;
  end Main;
end Program;
