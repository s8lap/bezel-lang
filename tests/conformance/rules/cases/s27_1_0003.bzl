//! case: s27_1_0003
//! doc: spec
//! section: 27.1
//! title: an owned-parameter function type is distinct from the unmarked form
//! rule: Function types may include the parameter modifier `owned`; `method
//!   Foo(owned t: T)` parallels `(owned T) -> ()`, and the modifier is part of
//!   the type.
//! cites: 27.1
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Registry
  private _reclaim: (owned Widget) -> () := lambda(owned w) => w.Dispose();

  public method Reclaim(owned w: Widget)
    _reclaim(move w);
  end Reclaim;
end Registry;
