//! case: l4_5_2501
//! doc: stdlib
//! section: 4.5
//! title: an untyped OwningArray.Sort comparer lambda over a class member sorts like the typed one
//! rule: // comparer takes borrows
//! cites: 27.4, 28.11
//! expect: run
//! output: True True
module Conf;
public class Item
  public Name: string;
  public constructor(n: string)
    Name := n;
  end constructor;
end Item;

public class Program
  public static method Main()
    using var typed := new OwningArray<Item>();
    using var untyped := new OwningArray<Item>();
    typed.Add(new Item("b"));
    typed.Add(new Item("c"));
    typed.Add(new Item("a"));
    untyped.Add(new Item("b"));
    untyped.Add(new Item("c"));
    untyped.Add(new Item("a"));
    typed.Sort(lambda(x: Item, y: Item): int => x.Name.CompareTo(y.Name));
    untyped.Sort(lambda(x, y) => x.Name.CompareTo(y.Name));
    var same := true;
    for var i in 0..<3 do
      if typed[i].Name <> untyped[i].Name then
        same := false;
      end if;
    end for;
    var moved := typed[0].Name <> "b";
    Console.WriteLine($"{same} {moved}");
  end Main;
end Program;
