//! case: s8_2_0002
//! doc: spec
//! section: 8.2
//! title: GetType cannot be overridden
//! rule: `GetType` is non-virtual; runtime type identity is fixed at
//!   allocation and cannot be overridden.
//! cites: 8.1, 8.2
//! expect: reject
//! diagnostic: non-virtual
module Conf;

public class Widget
  public constructor()
  end constructor;

  public override method GetType(): Type => typeof(Widget);
end Widget;
