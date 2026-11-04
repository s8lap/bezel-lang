//! case: s28_3_0339
//! doc: spec
//! section: 28.3
//! title: an owned nested case arm beside an unmarked named owned collection is a rejected mixed meet
//! rule: 3. owned ⊓ borrowed = **rejected**
//! cites: 28.3, 28.10, 19.2
//! expect: reject
//! diagnostic: borrow
//! issue: 8060
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
    using var a := Mk();
    using var u := (case i mod 2 of 0 => (case i of _ => new OwningArray<Item>()), _ => a);
    return u.Count;
  end Run;
end Program;
