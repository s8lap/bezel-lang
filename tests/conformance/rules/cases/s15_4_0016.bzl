//! case: s15_4_0016
//! doc: spec
//! section: 15.4
//! title: a fresh unbound owned temporary at a borrowing collection's Add draws the fresh-store warning
//! rule: Because the retention is statically certain to outlive the value, the
//!   compiler **must warn** when a store-position argument in a **borrowing
//!   position** — `Add`/`Insert` and the indexer-set on a borrowing-family
//!   collection, key and value positions alike, ... — is a **fresh unbound owned
//!   temporary of class type** (§15.4, v0.343).
//! cites: 15.4, 28.11
//! expect: warn
//! diagnostic: fresh unbound owned temporary
module Conf;

public class Item
  public constructor() end constructor;
  destructor Console.WriteLine("item disposed"); end destructor;
end Item;

public class Program
  public static method Main(): int
    var a: Array<Item> := [];
    a.Add(new Item());
    Console.WriteLine("after add");
    return 0;
  end Main;
end Program;
