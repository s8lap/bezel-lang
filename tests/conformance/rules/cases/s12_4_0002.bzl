//! case: s12_4_0002
//! doc: spec
//! section: 12.4
//! title: bare sealed on a member is rejected
//! rule: On a member, `sealed` appears only as the compound `sealed override`:
//!   a member declaration carrying `sealed` without `override` is a compile
//!   error, whatever the member's kind and whatever other dispatch modifier it
//!   carries.
//! cites: 12.4, 11.5
//! expect: reject
//! diagnostic: sealed
module Conf;

public class Widget
  public constructor()
  end constructor;

  public sealed method Describe(): string => "widget";
end Widget;
