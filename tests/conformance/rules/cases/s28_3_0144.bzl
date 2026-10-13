//! case: s28_3_0144
//! doc: spec
//! section: 28.3
//! title: an assignment made inside a lambda body counts toward the captured binding's set at its own return
//! rule: A captured binding — a local or parameter, in any state, that a lambda in
//!   the body captures — holds, for this count and wherever else its source set
//!   is read ... every value it is ever assigned ... its set is the union, over its initializer and every assignment to it
//!   in any body ... over `var g := lambda() => do s := zs.AsSpan(); end; g(); return s;`,
//!   `s` carries `zs` at the return
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
  private owned _a: Item;

  public constructor()
    _a := new Item();
  end constructor;

  public method Make(): Item
    var s := _a;
    using var z := new Item();
    var g := lambda() => do
      s := z;
    end;
    g();
    return s;
  end Make;
end Maker;
