//! case: s28_6_0014
//! doc: spec
//! section: 28.6
//! title: disposing an owned field read from outside its declaring class is rejected
//! rule: The borrow receivers for `Dispose()` include an owned-field read
//!   outside the declaring class — that read is a borrow, and disposing
//!   through it is rejected with no exception.
//! cites: 28.6
//! expect: reject
//! diagnostic: borrow
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
end Wrapped;

public class Outsider
  public static method ReleaseFrom(w: Wrapped)
    w._resource.Dispose();
  end ReleaseFrom;
end Outsider;
