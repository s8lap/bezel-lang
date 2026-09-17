//! case: s28_8_1_0004
//! doc: spec
//! section: 28.8.1
//! title: scoping the move to the branch where reassignment is mandatory satisfies the rule
//! rule: The fix is to scope the move to the branch where reassignment is
//!   reachable, so every path that performs the move also restores the
//!   field before the method's exit.
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

  public method Right2(replace: bool): owned Widget?
    if not replace then return null; end;
    var fresh := new Widget();
    var old := move _resource;
    _resource := fresh;
    return old;
  end Right2;
end Holder;
