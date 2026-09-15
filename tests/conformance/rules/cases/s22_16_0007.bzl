//! case: s22_16_0007
//! doc: spec
//! section: 22.16
//! title: result denotes the returned value in an ensures clause, read as a borrow
//! rule: Within an `ensures` clause of a member with a declared return
//!   type, `result` denotes the value being returned, typed as the
//!   declared return type, in a read-only borrow position.
//! cites: 22.16
//! expect: accept
module Conf;

public class Ops
  public static method Increment(x: int): int
    ensures(result = x + 1)

    return x + 1;
  end Increment;
end Ops;
