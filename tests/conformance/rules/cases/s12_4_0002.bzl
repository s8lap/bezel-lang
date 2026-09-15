//! case: s12_4_0002
//! doc: spec
//! section: 12.4
//! title: sealed on a non-virtual, non-override member is rejected
//! rule: sealed may not appear on a non-virtual, non-override member.
//! cites: 12.4
//! expect: reject
//! diagnostic: sealed
module Conf;

public class Widget
  public constructor()
  end constructor;

  public sealed method Describe(): string => "widget";
end Widget;
