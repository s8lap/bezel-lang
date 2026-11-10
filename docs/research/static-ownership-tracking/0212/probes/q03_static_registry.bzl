module Probe;
// KIND 2 in a static: a process-lived registry holds borrows of items that die earlier. EXPECT today: compiles; a report line at the collection store; checked = DisposedAccessException. Under 0212: the store adds `it` to a holder that static storage holds, and static storage admits only static-sourced borrows (28.3 Static stores): error at the store. The rewrite is q04.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Registry
  private static readonly _items: Array<Item> := new Array<Item>();
  public static method Register(it: Item) => _items.Add(it);
  public static method Dump()
    for var it in _items do
      Console.WriteLine(it.Name);
    end;
  end Dump;
end Registry;
public class Program
  public static method Main(): int
    do
      using var a := new Item("a");
      Registry.Register(a);
    end;
    Registry.Dump();
    return 0;
  end Main;
end Program;
