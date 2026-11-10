module Probe;
// STATIC REGIME (records): the source of a borrow-bearing record local is explicitly disposed, then the record is used (the kill dominates the use). EXPECT (spec §28.2, proposal 0205's holder rule): compile error at the use. The class analogue is k3a: admitted today, faults on every run.
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
    var i := new Item("i");
    var p := new Pair(i);
    i.Dispose();
    Console.WriteLine(p.Item!.Name);
    return 0;
  end Main;
end Program;
