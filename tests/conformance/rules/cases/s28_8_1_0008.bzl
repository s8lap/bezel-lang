//! case: s28_8_1_0008
//! doc: spec
//! section: 28.8.1
//! title: restructuring so the reassignment dominates the exits satisfies the loop case
//! rule: The fix is the same as the conditional case — restructure so the
//!   reassignment dominates the exits, moving the move after the loop.
//! cites: 28.8.1
//! expect: accept
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

  public method RightLoop(again: bool): owned Widget
    var fresh := new Widget();
    while again do
      fresh := new Widget();
      again := false;
    end;
    var old := move _resource;
    _resource := fresh;
    return old;
  end RightLoop;
end Holder;
