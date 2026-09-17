//! case: s23_5_0001
//! doc: spec
//! section: 23.5
//! title: an enum may not declare an indexer
//! rule: Enums may not declare indexers.
//! cites: 23.5, 10.5
//! expect: reject
//! diagnostic: indexer
module Conf;

public enum Color
  Red, Green, Blue;

  public property [i: int]: int => i;
end Color;
