//! case: s23_5_0003
//! doc: spec
//! section: 23.5
//! title: a ref parameter in an indexer parameter list is rejected
//! rule: `ref` and `out` parameters are not allowed in indexer parameter lists.
//! cites: 23.5, 22.3
//! expect: reject
//! diagnostic: ref
module Conf;

public class Holder
  public constructor()
  end constructor;

  public property [ref i: int]: int => i;
end Holder;
