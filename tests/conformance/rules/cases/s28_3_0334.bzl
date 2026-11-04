//! case: s28_3_0334
//! doc: spec
//! section: 28.3
//! title: a local bound to an owned case meet over collection producers is owned and returnable
//! rule: owned ⊓ owned = owned, obligation carried forward (for expression arms: exactly one arm's value exists at runtime, so exactly one obligation)
//! cites: 28.3, 19.2, 20
//! expect: run
//! issue: 8060
//! output: 5
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
    var u := (case i mod 2 of 0 => Mk(), _ => new OwningArray<Item>());
    return u;
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
