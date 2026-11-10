module Probe;
// KIND 2 -> KIND 3 by declaration order: the registry is declared AFTER the items, so it dies first. Spelled with today's `scoped` parameter on a method (bounding form 5: the receiver, a `using var`). EXPECT (spec §28.14): compiles; no report line. Under A the same program needs no marker at the call beyond what A's surface asks, and the same order is what the scope comparison checks.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Watch
  private _item: Item?;
  public constructor()
  end constructor;
  public method Attach(scoped item: Item) => _item := item;
  public method Show(): string => _item!.Name;
end Watch;
public class Program
  public static method Main(): int
    using var a := new Item("a");
    using var w := new Watch();
    w.Attach(scoped a);
    Console.WriteLine(w.Show());
    return 0;
  end Main;
end Program;
