module Probe;
// STATIC REGIME (records): the source of a live borrow-bearing record local is transferred (a silent kill). EXPECT (spec K5.1 through K5.5's holders): compile error at the move. The class analogue is k3b: admitted today, faults.
public class Item
  private _name: string;
  public constructor(name: string)
    _name := name;
  end constructor;
  public property Name: string => _name;
end Item;
public record Pair(Item: Item?);
public class Program
  public static method Take(owned i: Item) => i.Dispose();
  public static method Main(): int
    var i := new Item("i");
    var p := new Pair(i);
    Take(move i);
    Console.WriteLine(p.Item!.Name);
    return 0;
  end Main;
end Program;
