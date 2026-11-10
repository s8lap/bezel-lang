module Probe;
// KIND 2 under A: a long-lived registry declared BEFORE the items it observes cannot hold tracked borrows of them (p17's rule). The safe spelling that exists is Shared<T> + Weak<T>. EXPECT (spec §28.13): compiles; no report line; prints "gone" then "a"; never faults in either build mode.
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
  public method Sweep()
    for var w in _items do
      using var s := w.TryUpgrade();
      if s <> null then
        Console.WriteLine(s.Get().Name);
      else
        Console.WriteLine("gone");
      end;
    end;
  end Sweep;
end Registry;
public class Program
  public static method Main(): int
    using var reg := new Registry();
    using var a := new Shared<Item>(new Item("a"));
    do
      using var b := new Shared<Item>(new Item("b"));
      reg.Register(b.Downgrade());
    end;
    reg.Register(a.Downgrade());
    reg.Sweep();
    return 0;
  end Main;
end Program;
