//! case: s28_11_0040
//! doc: spec
//! section: 28.11
//! title: storing an owned parameter's borrow into a collection held by a plain parameter is rejected
//! rule: A borrow of a **scope-bound binding** — an `owned` parameter included, its
//!   value being disposed at the method's end — stored into a collection
//!   that provably outlives that scope — the collection is a field, is
//!   reachable from one, is held by a parameter that is not `owned` or
//!   reached through one (caller storage, which outlives every scope of the
//!   body), escapes the method, or is a local of strictly wider scope — is
//!   **rejected** at the store site.
//! cites: 28.11, 28.3
//! expect: reject
//! diagnostic: outlive
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Program
  public static method Store(a: Array<Item>, owned it: Item): int
    a.Add(it);
    return a.Count;
  end Store;

  public static method Main(): int
    var a: Array<Item> := [new Item()];
    Console.WriteLine(Program.Store(a, new Item()));
    return 0;
  end Main;
end Program;
