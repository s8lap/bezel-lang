//! case: s11_5_0002
//! doc: spec
//! section: 11.5
//! title: readonly before owned is rejected
//! rule: The compiler rejects out-of-order modifiers. owned appears between
//!   the virtual-modifier slot and readonly.
//! cites: 11.5
//! expect: reject
//! diagnostic: order
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cache
  public static readonly owned _instance: Widget := new Widget();

  public constructor()
  end constructor;
end Cache;
