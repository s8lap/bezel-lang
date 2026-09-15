//! case: s22_16_0004
//! doc: spec
//! section: 22.16
//! title: a contract clause may call only pure members
//! rule: Each clause's expression is of type `bool`, is admitted under
//!   the pure-body discipline of §22.15.
//! cites: 22.16, 22.15
//! expect: reject
//! diagnostic: pure
module Conf;

public class Box
  private _v: int := 0;

  public method Get(): int => _v;
end Box;

public class Ops
  public static method Touch(b: Box)
    requires(b.Get() > 0)

  end Touch;
end Ops;
