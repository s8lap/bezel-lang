//! case: s15_4_0016
//! doc: spec
//! section: 15.4
//! title: a fresh unbound owned temporary at a borrowing collection's Add is a compile error
//! rule: A fresh unbound owned temporary of class type ... — a `new` expression,
//!   a collection literal, an `owned`-returning call result, a `move` result, or
//!   a **`Task`-family call result** ... — in a single-element store position of
//!   a borrowing-family collection is a compile error at the store site
//!   (§15.4, v0.355).
//! cites: 15.4, 28.11
//! expect: reject
//! diagnostic: §15.4 fresh-store error
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
