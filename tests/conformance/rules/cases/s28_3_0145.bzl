//! case: s28_3_0145
//! doc: spec
//! section: 28.3
//! title: a slot store into a captured inline array after the closure is stored is counted in its set
//! rule: A captured binding — a local or parameter, in any state, that a lambda in
//!   the body captures — holds, for this count and wherever else its source set
//!   is read ... every value it is ever assigned ... a store through a member or index chain that lands in the binding's
//!   own storage (a slot of a captured inline array, §15.1.2; a settable member
//!   of a captured record, §9.4's own storage), which adds the stored value's
//!   sources as a slot store does
//! cites: 28.3, 15.1.2, 27.5
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
    var a: [1]Item? := [null];
    _f := lambda() => a[0]!.N();
    using var it := new Item();
    a[0] := it;
  end Arm;
end Holder;
