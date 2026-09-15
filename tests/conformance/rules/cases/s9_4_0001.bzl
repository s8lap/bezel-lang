//! case: s9_4_0001
//! doc: spec
//! section: 9.4
//! title: body-form properties may be declared with any mutability
//! rule: In the body form, properties may be declared with any mutability.
//!   Mutable records are permitted but are inadvisable.
//! expect: accept
module Conf;

public record Counter
  public property Value: int;
  public readonly property Name: string;

  public constructor(name: string, value: int)
    Name := name;
    Value := value;
  end constructor;
end Counter;
