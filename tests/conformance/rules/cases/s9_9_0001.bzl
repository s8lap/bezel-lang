//! case: s9_9_0001
//! doc: spec
//! section: 9.9
//! title: a record with a requires-carrying constructor is constructor-only and declares fine
//! rule: A record type is constructor-only when it declares a constructor
//!   carrying a contract clause: a requires clause, an ensures clause, or both,
//!   on any constructor of the record, of any visibility, in any recorded tier.
//! cites: 22.16
//! expect: accept
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;
