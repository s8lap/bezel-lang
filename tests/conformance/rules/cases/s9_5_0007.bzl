//! case: s9_5_0007
//! doc: spec
//! section: 9.5
//! title: with over a constructor-only record's source is rejected
//! rule: A with expression whose source expression's static type is a
//!   constructor-only record is a compile error at the with expression, naming
//!   the property and the source type.
//! cites: 9.9
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
  public static method Reset(p: Port): Port => p with(Value := 0);
end Program;
