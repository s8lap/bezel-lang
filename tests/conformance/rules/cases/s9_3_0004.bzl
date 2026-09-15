//! case: s9_3_0004
//! doc: spec
//! section: 9.3
//! title: a record declaring positional fields may not declare a constructor carrying a contract clause
//! rule: The body may not declare a constructor carrying a contract clause; the
//!   auto-generated primary constructor can carry none, so no record declaring
//!   positional fields is constructor-only.
//! cites: 9.9, 22.16
//! expect: reject
//! diagnostic: contract clause
module Conf;

public record Port(Value: int)
  public constructor(v: int, forced: bool)
    requires(v >= 0 and v <= 65535)

    Value := v;
  end constructor;
end Port;
