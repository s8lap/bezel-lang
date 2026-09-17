//! case: s9_9_0007
//! doc: spec
//! section: 9.9
//! title: constructor-only-record storage initialized by a constructor call is admitted
//! rule: The remedy is a constructor or factory call: the property claims
//!   provenance — every value of a constructor-only record is produced by a call
//!   to one of that record's declared constructors — and a field initializer
//!   that calls one satisfies definite assignment.
//! cites: 7.9, 17.11
//! expect: accept
module Conf;

public record Port
  public readonly property Value: int;

  public constructor(value: int)
    requires(value >= 0)

    Value := value;
  end constructor;
end Port;

public class Listener
  private _port: Port := new Port(8080);

  public constructor()
  end constructor;

  public method Number(): int => _port.Value;
end Listener;
