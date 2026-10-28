module Probe;
// Kind 4: the storing callee is reached through a function value (`var f := k.Keep;` then `f(it)`): the caller's view is a function type that announces no retention. EXPECT (spec): compiles; one report line at the store; DisposedAccessException in checked mode.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Keeper
  private _x: Item?;
  public constructor()
  end constructor;
  public method Keep(x: Item) => _x := x;
  public method Show(): string => _x!.Name;
end Keeper;
public class Program
  public static method Main(): int
    using var k := new Keeper();
    var f: (Item) -> () := k.Keep;
    var it := new Item("i");
    f(it);
    it.Dispose();
    Console.WriteLine(k.Show());
    return 0;
  end Main;
end Program;
