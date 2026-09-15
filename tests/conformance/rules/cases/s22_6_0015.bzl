//! case: s22_6_0015
//! doc: spec
//! section: 22.6
//! title: owned on an out parameter never distinguishes overloads
//! rule: "`owned` on an **`out` parameter** stays outside the
//!   distinguishability list: two methods differing only in the `owned`
//!   qualifier on an `out` parameter are a compile error."
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
