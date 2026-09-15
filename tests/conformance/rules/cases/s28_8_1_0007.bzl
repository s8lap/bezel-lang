//! case: s28_8_1_0007
//! doc: spec
//! section: 28.8.1
//! title: a loop that may execute zero times does not satisfy the field-restoration rule
//! rule: Loops that may execute zero times do not satisfy the rule — the
//!   analysis reuses the definite-assignment dominance machinery, so a
//!   restoration reachable only through a conditionally-taken loop body
//!   does not dominate the exit.
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

  public method WrongLoop(again: bool): owned Widget
    var old := move _resource;
    while again do
      _resource := new Widget();
      again := false;
    end;
    return old;
  end WrongLoop;
end Holder;
