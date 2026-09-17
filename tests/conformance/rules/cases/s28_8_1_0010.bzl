//! case: s28_8_1_0010
//! doc: spec
//! section: 28.8.1
//! title: move on a field is permitted after its first constructor assignment
//! rule: After the first assignment, `move` is permitted, with the same
//!   field-restoration rule applying to constructor exit paths.
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
    var old := move _resource;
    _resource := new Widget();
    old.Dispose();
  end constructor;
end Holder;
