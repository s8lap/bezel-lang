//! case: s9_8_0005
//! doc: spec
//! section: 9.8
//! title: a record component that declares neither serialization interface is not admissible
//! rule: A component type is admissible as a serialized component when it is
//!   another serialization-eligible record **that itself declares
//!   `ISerializable`/`IDeserializable`**: the synthesized members reach the
//!   nested value's own `Serialize`/`Deserialize`, which the conformance is the
//!   sole source of, so a record shaped eligibly that declares neither
//!   interface is not an admissible component and naming one is the
//!   conformance-declaration error.
//! cites: 9.8, 3.6
//! expect: reject
//! diagnostic: admissible
//! issue: 4398
module Conf;

public record Inner
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
