//! case: s28_11_0024
//! doc: spec
//! section: 28.11
//! title: a local copy and a receiver-sourced snapshot are admitted
//! rule: So `_all.AddRange(items)` from a plain parameter and `return _items.ToArray()`
//!   over a field are admitted under the residual, and `_all.AddRange(local)` and
//!   `return local.ToArray()` over a scope-bound local are rejected, as
//!   `_all.Add(local[0])` is ... A copy is judged as a store of each placed value into the destination, at
//!   the call — at a class or interface element type under this rule, at a value
//!   type that holds borrows under §9.7's collection bullet
//! cites: 28.11, 9.7
//! expect: run
//! stdout: 2
//! issue: 5721
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public record Pair(Ref: Item);

public class Bag
  private owned _items: Array<Item>;

  public constructor()
    _items := new Array<Item>();
  end constructor;

  public method Put(x: Item)
    _items.Add(x);
  end Put;

  public method Snapshot(): owned Array<Item> => _items.ToArray();
end Bag;

public class Program
  public static method Main()
    using var it := new Item();
    var xs: Array<Pair> := [new Pair(it)];
    using var ys := xs.ToArray();
    using var bag := new Bag();
    bag.Put(it);
    using var snap := bag.Snapshot();
    Console.WriteLine((ys.Count + snap.Count).ToString());
  end Main;
end Program;
