//! case: s28_11_0032
//! doc: spec
//! section: 28.11
//! title: a local collection holding a borrow may move within its element's block
//! rule: a borrow of a scope-bound binding ... stored into a collection that
//!   provably outlives that scope — the collection is a field, is reachable from
//!   one, is held by a parameter that is not `owned` or reached through one
//!   (caller storage, which outlives every scope of the body), escapes the
//!   method, or is a local of strictly wider scope — is rejected at the store
//!   site
//! cites: 28.11, 9.7
//! expect: run
//! stdout: 1
//! issue: 5735
module Conf;

public class Item
  public constructor()
  end constructor;

  public method N(): int => 1;
end Item;

public class Program
  public static method Main()
    using var item := new Item();
    var coll := new Array<Item>();
    coll.Add(item);
    var c2 := move coll;
    Console.WriteLine(c2.Count.ToString());
  end Main;
end Program;
