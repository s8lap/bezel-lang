//! case: s23_5_0002
//! doc: spec
//! section: 23.5
//! title: a static indexer is rejected
//! rule: Static indexers are not supported.
//! cites: 23.5
//! expect: reject
//! diagnostic: static
module Conf;

public class Lookup
  public constructor()
  end constructor;

  public static property [i: int]: int => i;
end Lookup;
