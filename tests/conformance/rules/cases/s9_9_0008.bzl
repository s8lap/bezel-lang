//! case: s9_9_0008
//! doc: spec
//! section: 9.9
//! title: a record declaring positional fields may not declare a contract-clause constructor
//! rule: A record declaring positional fields may not declare a constructor
//!   carrying a contract clause: a compile error at the record declaration. The
//!   auto-generated primary constructor carries no clause, so the combination
//!   would leave `new R(…)` outside every gate the declared constructor states.
//! cites: 9.1, 9.3, 22.16
//! expect: reject
//! diagnostic: positional
module Conf;

public record Port(Value: int)
  public constructor(low: int, high: int)
    requires(low <= high)

    Value := low;
  end constructor;
end Port;
