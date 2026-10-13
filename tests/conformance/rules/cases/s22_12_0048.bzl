//! case: s22_12_0048
//! doc: spec
//! section: 22.12
//! title: a slot store through a static readonly owning inline array is rejected
//! rule: Its slots are fixed at initialization: a slot store or a `move arr[j]`
//!   through it is a compile error naming K9.4 — static owned storage is
//!   disposed only by the exit pass, and no per-method window can cover it.
//! cites: 22.12, 15.1.1, 28.3
//! expect: reject
//! diagnostic: K9.4
//! issue: 5807
module Conf;

public class Connection
  public constructor()
  end constructor;
end Connection;

public class Registry
  public static readonly Pool: [2 owned]Connection?;

  public static method Refill()
    Registry.Pool[0] := new Connection();
  end Refill;
end Registry;
