//! case: s28_8_1_0002
//! doc: spec
//! section: 28.8.1
//! title: a terminal move is rejected even under an owned return
//! rule: There is no terminal-move dispensation — a terminal move
//!   (`return move x.f;`, or any return whose operand evaluation performs
//!   the move with no restoring assignment before the exit) is rejected,
//!   the return edge being itself an unrestored exit.
//! cites: 28.8.1
//! expect: reject
//! diagnostic: not reassigned
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method Take(): owned Widget
    return move _resource;
  end Take;
end Holder;
