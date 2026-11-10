module Probe;
// STATIC REGIME (records): a static method returns a borrow-bearing record carrying a plain parameter. EXPECT (spec §9.7 return bullet): compile error at the return. The class analogue — a factory returning `new Account(c)` — is admitted on trust today (k3d).
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public record Pair(Item: Item?);
public class Program
  public static method Wrap(i: Item): Pair => new Pair(i);
  public static method Main(): int
    using var i := new Item("i");
    var p := Wrap(i);
    Console.WriteLine(p.Item!.Name);
    return 0;
  end Main;
end Program;
