module Probe;
// Kind 2: a long-lived registry holds borrows of items it does not own (collection-held clause). An item is disposed while registered; the registry's later sweep touches it. EXPECT (spec): compiles; one report line at `_items.Add(item)`; DisposedAccessException in checked mode at the sweep.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Registry
  private owned _items: Array<Item> := new Array<Item>();
  public constructor()
  end constructor;
  public method Register(item: Item) => _items.Add(item);
  public method Names(): int
    var n := 0;
    for var it in _items do
      Console.WriteLine(it.Name);
      n := n + 1;
    end;
    return n;
  end Names;
end Registry;
public class Program
  public static method Main(): int
    using var reg := new Registry();
    using var a := new Item("a");
    var b := new Item("b");
    reg.Register(a);
    reg.Register(b);
    b.Dispose();
    return reg.Names();
  end Main;
end Program;
