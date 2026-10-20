//! case: s35_11_0003
//! doc: spec
//! section: 35.11
//! title: a generic choice at a borrow-bearing record payload is borrow-bearing and = is rejected
//! rule: An *unmarked* bare-type-parameter payload is classification-neutral
//!   at a runtime-managed substitution and at a copyable value substitution
//!   that carries no borrow ... and **borrow-carrying** at a class or
//!   interface substitution and at a value substitution that holds borrows
//!   (... so `Opt<Pair>` over `record Pair(It: Item)` is borrow-bearing by
//!   rule 2 and draws §35.7's use-site error on `=`)
//! cites: 35.11, 35.4, 35.7
//! expect: reject
//! diagnostic: auto-hashable
module Conf;

public class Item
  public constructor()
  end constructor;
end Item;

public record Pair(It: Item);

public choice Opt<T>
  Some(value: T),
  None;
end Opt;

public class Program
  public static method Main()
    using var it := new Item();
    var p := new Pair(it);
    var a := Opt<Pair>.Some(p);
    var b := Opt<Pair>.Some(p);
    Console.WriteLine(a = b);
  end Main;
end Program;
