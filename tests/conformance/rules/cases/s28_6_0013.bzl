//! case: s28_6_0013
//! doc: spec
//! section: 28.6
//! title: disposing an owned field from within its declaring class is admitted
//! rule: Explicit disposal requires an owned receiver; an `owned` field read
//!   by code of the owning object's declaring type is such a receiver — unlike
//!   `this`, which an object may never dispose out from under its owner.
//! cites: 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Session
  private owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public method Close()
    _resource.Dispose();
  end Close;
end Session;
