//! case: s28_3_0336
//! doc: spec
//! section: 28.3
//! title: a nested if/case meet over collection producers is an owned arm returnable from an owned method
//! rule: owned ⊓ owned = owned, obligation carried forward (for expression arms: exactly one arm's value exists at runtime, so exactly one obligation)
//! cites: 28.3, 28.10, 19.2, 20
//! expect: run
//! issue: 8060
//! output: 4
//!   0
module Conf;

public class Item
  public static live: int := 0;

  public constructor()
    Item.live := Item.live + 1;
  end constructor;

  destructor Item.live := Item.live - 1; end destructor;
end Item;

public class Program
  public static method Mk(): owned OwningArray<Item>
    var a := new OwningArray<Item>();
    a.Add(new Item());
    return a;
  end Mk;

  public static method Pick(i: int): owned OwningArray<Item>
    return (if i > 5 then (case i of _ => Mk()) else new OwningArray<Item>());
  end Pick;

  public static method Run(i: int): int
    using var p := Pick(i);
    return p.Count;
  end Run;

  public static method Main()
    var total := 0;
    for var i in 0..<10 do
      total := total + Run(i);
    end for;
    Console.WriteLine(total);
    Console.WriteLine(Item.live);
  end Main;
end Program;
