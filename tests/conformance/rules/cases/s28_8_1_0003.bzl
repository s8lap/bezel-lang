//! case: s28_8_1_0003
//! doc: spec
//! section: 28.8.1
//! title: a field moved on one branch and not reassigned on the other is rejected
//! rule: The rule applies to every reachable exit; the compiler rejects a
//!   move whose restoring assignment is missing on some path to the
//!   method's exit.
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

  public method Wrong(replace: bool): owned Widget
    var old := move _resource;
    if replace then
      _resource := new Widget();
    end;
    return old;
  end Wrong;
end Holder;
