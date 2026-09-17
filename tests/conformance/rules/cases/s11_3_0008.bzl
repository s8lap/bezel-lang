//! case: s11_3_0008
//! doc: spec
//! section: 11.3
//! title: owned on an out parameter never distinguishes overloads
//! rule: owned on an out parameter and scoped never distinguish overloads;
//!   two methods differing only in the owned qualifier on an out parameter
//!   are a compile error (§22.6).
//! cites: 11.3, 22.6
//! expect: reject
//! diagnostic: clash
module Conf;

public class Widget
  public constructor()
  end constructor;
end Widget;

public class Cache
  public constructor()
  end constructor;

  public method TryGet(key: string, out value: Widget?): bool
    value := null;
    return true;
  end TryGet;

  public method TryGet(key: string, out owned value: Widget?): bool
    value := null;
    return true;
  end TryGet;
end Cache;
