//! case: s22_6_0015
//! doc: spec
//! section: 22.6
//! title: owned on an out parameter never distinguishes overloads
//! rule: **`owned` never distinguishes overloads.** Two same-name members
//!   that differ only by `owned` on a parameter — an ordinary parameter or an
//!   `out` parameter — are indistinguishable and clash at the second
//!   declaration (§11.3) ... as is `TryGet(name: string, out c: Conn?)`
//!   beside `TryGet(name: string, out owned c: Conn?)`
//! cites: 22.6
//! expect: reject
//! diagnostic: owned qualifier
module Conf;

public class Conn
  public constructor()
  end constructor;
end Conn;

public class Pool
  public method TryGet(name: string, out c: Conn?): bool
    c := null;
    return false;
  end TryGet;

  public method TryGet(name: string, out owned c: Conn?): bool
    c := null;
    return false;
  end TryGet;
end Pool;
