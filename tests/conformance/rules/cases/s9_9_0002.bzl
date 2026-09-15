//! case: s9_9_0002
//! doc: spec
//! section: 9.9
//! title: default on a constructor-only record is rejected
//! rule: A constructor-only record is not defaultable; default(R) and every
//!   other site that supplies a default(T) are rejected.
//! cites: 17.11
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

public class Program
  public static method Zero(): Port => default(Port);
end Program;
