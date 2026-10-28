module Probe;
// Kind 4: the store is behind interface dispatch; the caller sees only `ISink.Keep(x: Item)`. No body-derived fact can reach the caller through the interface (the implementation may be in another module). EXPECT (spec): compiles; one report line at the implementation's store; DisposedAccessException in checked mode at `Show()`.
public interface ISink
  method Keep(x: Item);
  method Show(): string;
end ISink;
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public class Keeper: ISink
  private _x: Item?;
  public constructor()
  end constructor;
  public method Keep(x: Item) => _x := x;
  public method Show(): string => _x!.Name;
end Keeper;
public class Program
  public static method Feed(sink: ISink, it: Item) => sink.Keep(it);
  public static method Main(): int
    using var k := new Keeper();
    var it := new Item("i");
    Feed(k, it);
    it.Dispose();
    Console.WriteLine(k.Show());
    return 0;
  end Main;
end Program;
