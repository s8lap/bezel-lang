//! case: s9_8_0006
//! doc: spec
//! section: 9.8
//! title: the same record component declaring the conformance is admissible
//! rule: The conformance requirement on the record row is what makes the
//!   component encodable: a nested record that declares
//!   `ISerializable`/`IDeserializable` supplies the members the synthesized
//!   bodies reach, and is then admissible as a serialized component.
//! cites: 9.8
//! expect: accept
//! issue: 4398
module Conf;

public record Inner: ISerializable, IDeserializable
  public readonly property A: int;

  public constructor(a: int)
    A := a;
  end constructor;
end Inner;

public record Outer: ISerializable, IDeserializable
  public readonly property Part: Inner;

  public constructor(part: Inner)
    Part := part;
  end constructor;
end Outer;
