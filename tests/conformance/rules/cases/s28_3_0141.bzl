//! case: s28_3_0141
//! doc: spec
//! section: 28.3
//! title: a closure stored before its captured binding is assigned a using var is rejected at the store
//! rule: A captured binding — a local or parameter, in any state, that a lambda in
//!   the body captures — holds, for this count and wherever else its source set
//!   is read ... every value it is ever assigned ... Over `var b: Item? := null; var f := lambda() => b; _f := f; using var it := new Item(); b := it;`,
//!   `f`'s set is `{it}`, so `_f := f;` is rejected where it stands
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
    var b: Item? := null;
    var f := lambda() => b!.N();
    _f := f;
    using var it := new Item();
    b := it;
  end Arm;
end Holder;
