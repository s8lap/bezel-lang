//! case: s11_5_0001
//! doc: spec
//! section: 11.5
//! title: the canonical modifier order is admitted on a static owned field
//! rule: visibility static (async|pure) (abstract|virtual|override|sealed
//!   override) (owned|scoped) readonly member-kind Name ... The combination
//!   static owned readonly is the canonical form for a static field that
//!   owns a long-lived resource.
//! cites: 11.5, 22.13
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cache
  public static owned readonly _instance: Widget := new Widget();

  public constructor()
  end constructor;
end Cache;
