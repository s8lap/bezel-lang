//! case: s28_3_0338
//! doc: spec
//! section: 28.3
//! title: a nested if/case meet over collection producers is accepted into an owned field
//! rule: owned ⊓ owned = owned, obligation carried forward (for expression arms: exactly one arm's value exists at runtime, so exactly one obligation)
//! cites: 28.3, 28.5, 19.2, 20
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

public class Holder
  public owned g: OwningArray<Item> := new OwningArray<Item>();
end Holder;

public class Program
  public static method Mk(): owned OwningArray<Item>
    var a := new OwningArray<Item>();
    a.Add(new Item());
    return a;
  end Mk;

  public static method Fill(): int
    using var h := new Holder();
    var total := 0;
    for var i in 0..<10 do
      h.g := (if i > 5 then (case i of _ => Mk()) else new OwningArray<Item>());
      total := total + h.g.Count;
    end for;
    return total;
  end Fill;

  public static method Main()
    Console.WriteLine(Fill());
    Console.WriteLine(Item.live);
  end Main;
end Program;
