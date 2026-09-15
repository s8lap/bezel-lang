//! case: s26_3_0004
//! doc: spec
//! section: 26.3
//! title: an extension block may not declare an indexer
//! rule: An extension block declares no indexers.
//! cites: 26.3, 23.4
//! expect: reject
//! diagnostic: indexer
module Conf;

public class Widget
  public property Size: int;

  public constructor(size: int)
    Size := size;
  end constructor;
end Widget;

extension Widget
  public property [i: int]: int => this.Size + i;
end;
