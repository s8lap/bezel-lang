module Probe;
// STATIC REGIME (records): a borrow-bearing record stored in a class instance field. EXPECT (spec §9.7 instance-field bullet): compile error. This is the rule candidate A would replace, for a class holder, by "the class becomes borrow-bearing".
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public record Pair(Item: Item?);
public class Holder
  private _pair: Pair := new Pair(null);
  public constructor()
  end constructor;
  public method Keep(i: Item) => _pair := new Pair(i);
  public method Show(): string => _pair.Item!.Name;
end Holder;
public class Program
  public static method Main(): int
    using var h := new Holder();
    using var i := new Item("i");
    h.Keep(i);
    Console.WriteLine(h.Show());
    return 0;
  end Main;
end Program;
