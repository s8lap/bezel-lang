module Probe;
// KIND 2 with today's `scoped`: the holder is declared BEFORE the source. EXPECT (spec §28.14 declared-before test): compile error at the scoped argument. This is the rejection candidate A gives every tracked holder; the program must reorder, own the item, or use Weak<T>.
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
    using var w := new Watch();
    using var a := new Item("a");
    w.Attach(scoped a);
    Console.WriteLine(w.Show());
    return 0;
  end Main;
end Program;
