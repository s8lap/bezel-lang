module Probe;
// Kind 2, today's safe spelling: items become `Shared<Item>`, the registry keeps `Weak<Item>` observers and reads them through `TryUpgrade()`, which answers null for a dead item. EXPECT (spec): compiles; no report line; prints "a" then "gone"; never faults in either build mode.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Registry
  private owned _items: OwningArray<Weak<Item>> := new OwningArray<Weak<Item>>();
  public constructor()
  end constructor;
  public method Register(owned w: Weak<Item>) => _items.Add(w);
  public method Sweep(): int
    var live := 0;
    for var w in _items do
      using var s := w.TryUpgrade();
      if s <> null then
        Console.WriteLine(s.Get().Name);
        live := live + 1;
      else
        Console.WriteLine("gone");
      end;
    end;
    return live;
  end Sweep;
end Registry;
public class Program
  public static method Main(): int
    using var reg := new Registry();
    using var a := new Shared<Item>(new Item("a"));
    var b := new Shared<Item>(new Item("b"));
    reg.Register(a.Downgrade());
    reg.Register(b.Downgrade());
    b.Dispose();
    return reg.Sweep();
  end Main;
end Program;
