//! case: s25_1_0002
//! doc: spec
//! section: 25.1
//! title: not is not user-overloadable
//! rule: `not` is not user-overloadable. It is the boolean-negation operator and
//!   applies only to `bool`; a domain-specific inversion is a named method.
//! cites: 25.1
//! expect: reject
//! diagnostic: not
module Conf;

public record TriBool(Value: int)
  public static method operator not(t: TriBool): TriBool => new TriBool(1 - t.Value);
end TriBool;
