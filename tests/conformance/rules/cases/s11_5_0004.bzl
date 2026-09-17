//! case: s11_5_0004
//! doc: spec
//! section: 11.5
//! title: static before visibility is rejected
//! rule: visibility static (async|pure) (abstract|virtual|override|sealed
//!   override) (owned|scoped) readonly member-kind Name ... The compiler
//!   rejects out-of-order modifiers.
//! cites: 11.5
//! expect: reject
//! diagnostic: order
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cache
  static public owned readonly _instance: Widget := new Widget();

  public constructor()
  end constructor;
end Cache;
