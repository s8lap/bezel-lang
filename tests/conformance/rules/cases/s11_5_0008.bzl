//! case: s11_5_0008
//! doc: spec
//! section: 11.5
//! title: owned is rejected on a non-field member
//! rule: owned ... appears between the virtual-modifier slot and readonly.
//!   It applies only to fields; methods, properties, and other members do
//!   not carry owned.
//! cites: 11.5
//! expect: reject
//! diagnostic: fields
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Factory
  private owned _cached: Widget := new Widget();

  public constructor()
  end constructor;

  public owned method Fetch(): Widget => _cached;

  destructor
    _cached.Dispose();
  end destructor;
end Factory;
