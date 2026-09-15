//! case: s8_6_0001
//! doc: spec
//! section: 8.6
//! title: a move-only owning inline array cannot be boxed
//! rule: Move-only value types — owning inline arrays (`[N owned]T` and
//!   `[N owned]T?`) — cannot be boxed. Boxing a value type copies the value
//!   into a heap wrapper, but move-only types forbid copying.
//! cites: 8.6, 15.1.1, 15.3
//! expect: reject
//! diagnostic: cannot be boxed
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Holder
  public constructor()
    var conns: [2 owned]Connection := [new Connection(), new Connection()];
    var o: Object := conns;
  end constructor;
end Holder;
