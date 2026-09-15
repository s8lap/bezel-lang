//! case: s8_6_0002
//! doc: spec
//! section: 8.6
//! title: a non-owning inline array boxes per the general rule
//! rule: Non-owning inline arrays (`[N]T` and `[N]T?`) are ordinary value
//!   types and box per the general rule.
//! cites: 8.6, 15.1
//! expect: accept
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Holder
  public constructor()
    var a := new Connection();
    var b := new Connection();
    var conns: [2]Connection := [a, b];
    var o: Object := conns;
  end constructor;
end Holder;
