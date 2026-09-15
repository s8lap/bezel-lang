//! case: s3_6_0004
//! doc: spec
//! section: 3.6
//! title: a constructor-only record is not defaultable
//! rule: constructor-only classifies records declaring a constructor carrying
//!   a contract clause; such a record is not defaultable.
//! cites: 3.6, 9.9, 17.11
//! expect: reject
//! diagnostic: constructor-only
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0 and value <= 65535)

    Value := value;
  end constructor;
end Port;

public class Holder
  public method Compute()
    var p := default(Port);
  end Compute;
end Holder;
