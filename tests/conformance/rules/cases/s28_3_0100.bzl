//! case: s28_3_0100
//! doc: spec
//! section: 28.3
//! title: a bare-T result lent from a lambda capturing a plain parameter may be stored in a field
//! rule: A function-typed argument bound to a source parameter whose declared
//!   type, nullability stripped, is a function type that mentions the
//!   returned parameter in its return slot or in a `ref` or `out` parameter
//!   slot, other than a slot declared `owned` (`f: () -> T`, `f: (T) -> T`,
//!   `f: () -> () -> T`, `f: (out T) -> bool`), and the function value of a
//!   function-value invocation, contribute their source sets. `static method
//!   Apply<T>(f: () -> T): T => f();` therefore lends, at `T = Item`, from
//!   what its argument captures: the result of `Apply<Item>(lambda() => x)`
//!   is usable only where `x` is live.
//! cites: 28.3
//! expect: accept
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

  public method Fill(p: Item)
    _v := Fns.Apply<Item>(lambda() => p);
  end Fill;
end Holder;
