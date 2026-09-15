//! case: s15_3_0002
//! doc: spec
//! section: 15.3
//! title: a move-only value type may not be a field of a record
//! rule: A move-only value may not be a field of a record — records support
//!   only copy semantics, not move semantics (§15.3).
//! cites: 15.3
//! expect: reject
//! diagnostic: record
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public record Wrapper(Items: [2 owned]Connection);
