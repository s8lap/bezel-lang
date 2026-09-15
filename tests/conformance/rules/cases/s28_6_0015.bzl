//! case: s28_6_0015
//! doc: spec
//! section: 28.6
//! title: disposing another instance's owned field is admitted from the same declaring class
//! rule: The field admission is by declaring type, matching §13.1's levels:
//!   code of class C may dispose an owned field of any C instance it can
//!   reach — the same access symmetry as any private-member touch.
//! cites: 13.1, 28.6
//! expect: accept
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Wrapped
  public owned _resource: Widget;

  public constructor()
    _resource := new Widget();
  end constructor;

  public static method ReleaseFrom(w: Wrapped)
    w._resource.Dispose();
  end ReleaseFrom;
end Wrapped;
