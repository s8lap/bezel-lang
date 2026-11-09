//! case: s8_6_0002
//! doc: spec
//! section: 8.6
//! title: a borrow-bearing inline array is refused at an `Object` position
//! rule: An inline array without the `owned` marker is borrow-bearing
//!   exactly when its element type, nullability stripped, is a class or
//!   interface type that is not runtime-managed (§15.1.2), so `[2]Connection`
//!   holds two borrows. A value that holds borrows may be converted to
//!   `Object` at exactly one position — the receiver of a member call — and
//!   every other conversion, at a local among them, is a compile error at the
//!   conversion (§9.7).
//! cites: 8.6, 15.1, 15.1.2, 9.7
//! expect: reject
//! diagnostic: borrow-bearing
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
