//! case: s28_11_0041
//! doc: spec
//! section: 28.11
//! title: storing a plain parameter's borrow into a collection held by a plain parameter is admitted
//! rule: Storing a class-typed borrow into a borrowing collection follows the same
//!   shape as §28.3's borrowed-field rule; parameter-sourced (a parameter
//!   that is not `owned`), receiver-sourced, and field-sourced borrows are
//!   admitted at the store site.
//! cites: 28.11, 28.3
//! expect: accept
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public class Program
  public static method Store(a: Array<Item>, it: Item): int
    a.Add(it);
    return a.Count;
  end Store;
end Program;
