//! case: s28_3_0099
//! doc: spec
//! section: 28.3
//! title: a bare-T result lent from a lambda's capture may not be stored past the captured local
//! rule: What each operand contributes: ... a function-typed argument bound
//!   to a source parameter whose declared type, nullability stripped, is a
//!   function type that mentions the returned parameter in its return slot or
//!   in a `ref` or `out` parameter slot, other than a slot declared `owned`
//!   (`f: () -> T`, `f: (T) -> T`, `f: () -> () -> T`, `f: (out T) -> bool`),
//!   and the function value of a function-value invocation, their source sets
//!   ... `static method Apply<T>(f: () -> T): T => f();` lends, at `T =
//!   Item`, from what its argument captures ... The result is usable only
//!   where every contributing source is still live
//! cites: 28.3, 9.7
//! expect: reject
//! diagnostic: outlive
//! issue: 5609
module Conf;

public class Item
  public constructor()
  end constructor;

  public method Size(): int => 1;
end Item;

public class Fns
  public static method Apply<T>(f: () -> T): T => f();
end Fns;

public class Holder
  private _v: Item?;

  public constructor()
  end constructor;

  public method Fill()
    using var x := new Item();
    _v := Fns.Apply<Item>(lambda() => x);
  end Fill;
end Holder;
