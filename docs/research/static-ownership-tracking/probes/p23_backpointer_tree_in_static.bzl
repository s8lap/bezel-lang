module Probe;
// KIND 1 under A: an owner whose owned child points back at it, held in a static. The only borrow inside the tree is of the tree itself. EXPECT (spec): compiles; one report line; prints Root. Under A the Owner class is borrow-bearing by type, but a value constructed from no borrowed argument has an EMPTY source set, so the static store is admitted (the class analogue of §9.7's null-source form).
public class Owner
  private _name: string;
  private owned _child: Child?;
  public constructor(name: string)
    _name := name;
    _child := new Child(this);
  end constructor;
  public property Name: string => _name;
  public method ChildOwner(): string => _child!.OwnerName();
end Owner;
public class Child
  private _owner: Owner;
  public constructor(owner: Owner)
    _owner := owner;
  end constructor;
  public method OwnerName(): string => _owner.Name;
end Child;
public class App
  public static readonly Root: Owner := new Owner("Root");
end App;
public class Program
  public static method Main(): int
    Console.WriteLine(App.Root.ChildOwner());
    return 0;
  end Main;
end Program;
