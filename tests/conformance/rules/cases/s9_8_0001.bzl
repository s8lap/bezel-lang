//! case: s9_8_0001
//! doc: spec
//! section: 9.8
//! title: a record with only admissible primitive and string components is serialization-eligible
//! rule: A record or enum is serialization-eligible when it satisfies the
//!   synthesis-eligibility rule; declaring the conformance is the opt-in, and the
//!   synthesis is automatic when eligible.
//! expect: accept
module Conf;

public record Metric: ISerializable, IDeserializable
  public readonly property Name: string;
  public readonly property Value: int;

  public constructor(name: string, value: int)
    Name := name;
    Value := value;
  end constructor;
end Metric;
