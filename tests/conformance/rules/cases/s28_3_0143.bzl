//! case: s28_3_0143
//! doc: spec
//! section: 28.3
//! title: a captured function-typed local later assigned a capturing lambda taints the closure that calls it
//! rule: A lambda literal contributes the sources of the borrows it captures ... a
//!   captured function value its own set, transitively ... its set is the union,
//!   over its initializer and every assignment to it in any body ... whatever
//!   the assignment's position relative to the capture or to the read
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

public class Holder
  private _f: (() -> int)?;

  public constructor()
  end constructor;

  public method Arm()
    var g := lambda() => 0;
    var f := lambda() => g();
    _f := f;
    using var it := new Item();
    g := lambda() => it.N();
  end Arm;
end Holder;
