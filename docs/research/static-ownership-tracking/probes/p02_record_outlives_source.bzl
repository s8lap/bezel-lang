module Probe;
// STATIC REGIME (records): a borrow-bearing record local declared in an outer scope takes a borrow of an inner-scope source. EXPECT (spec §9.7 first bullet + K2.3): compile error "would outlive its source". The class analogue (p17) is admitted on trust today.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public record Pair(Item: Item?);
public class Program
  public static method Main(): int
    var p := new Pair(null);
    do
      using var inner := new Item("inner");
      p := new Pair(inner);
    end;
    Console.WriteLine(p.Item!.Name);
    return 0;
  end Main;
end Program;
