//! case: s28_8_1_0009
//! doc: spec
//! section: 28.8.1
//! title: moving a field before its first constructor assignment is a definite-assignment violation
//! rule: Inside a constructor body, the field has no value to move until it
//!   has been assigned; a `move x.f` before the field's first assignment is
//!   a definite-assignment violation, the same rule as for any other read
//!   of an unassigned non-nullable slot.
//! cites: 28.8.1, 7.9
//! expect: reject
//! diagnostic: definite-assignment
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Holder
  private owned _resource: Widget;

  public constructor()
    var old := move _resource;
    _resource := new Widget();
  end constructor;
end Holder;
