//! case: s35_4_0009
//! doc: spec
//! section: 35.4
//! title: an owning choice may not be a record field
//! rule: Owning choices are move-only value types and every section 15.3 rule
//!   applies as written: no move-only value may be a record field (records
//!   support only copy semantics, not move semantics).
//! cites: 15.3, 9.7
//! expect: reject
//! diagnostic: record
module Conf;

public class Resource
  public constructor()
  end constructor;
end Resource;

public choice Holding
  Held(owned res: Resource),
  Nothing;
end Holding;

public record Box
  public readonly property Content: Holding;

  public constructor(content: Holding)
    Content := content;
  end constructor;
end Box;
