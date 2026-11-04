//! case: s28_10_0078
//! doc: spec
//! section: 28.10
//! title: a using var bound to a case of nested owned if/case arms disposes the selected collection
//! rule: An expression whose arm-meet is **owned** — a `??`, an `if` expression, or an expression-position `case` over owned producers
//! cites: 28.10, 28.3, 19.2, 20
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

  public static method Run(i: int): int
    using var u := (case i mod 3 of
      0 => (if i > 5 then Mk() else new OwningArray<Item>()),
      1 => Mk(),
      _ => (case i of _ => new OwningArray<Item>()));
    return u.Count;
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
