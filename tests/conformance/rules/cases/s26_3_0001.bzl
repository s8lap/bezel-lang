//! case: s26_3_0001
//! doc: spec
//! section: 26.3
//! title: an extension block may not declare a field
//! rule: An extension block declares only methods and computed read-only
//!   properties: no fields, auto-properties, constructors, destructors, or `owned`
//!   fields.
//! cites: 26.3
//! expect: reject
//! diagnostic: field
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

extension Widget
  private _count: int := 0;
  public method Count(): int => _count;
end;
