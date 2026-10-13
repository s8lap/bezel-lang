//! case: s28_3_0147
//! doc: spec
//! section: 28.3
//! title: two captured function-typed locals calling each other carry the sources their assignments bring in
//! rule: Where these sets depend on one another through captures — a lambda
//!   assigned to a binding it captures (§27.5's recursive closure), or two
//!   captured function-typed locals each assigned a lambda that calls the other
//!   — they are the least sets that satisfy this clause: a cycle adds nothing
//!   beyond the sources its assignments carry in from outside it
//! cites: 28.3, 27.5
//! expect: reject
//! diagnostic: outlive
//! issue: 5718
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Maker
  public static method Make(): () -> Item
    var f: (() -> Item)? := null;
    var g := lambda() => f!();
    using var it := new Item();
    f := lambda() => do
      var x := g();
      return it;
    end;
    return g;
  end Make;
end Maker;
