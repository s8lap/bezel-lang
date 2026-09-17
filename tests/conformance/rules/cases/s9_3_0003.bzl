//! case: s9_3_0003
//! doc: spec
//! section: 9.3
//! title: a combined-form body may not declare a property with a positional field's auto-generated name
//! rule: The body may not declare a property with the same name as a positional
//!   field's auto-generated property.
//! expect: reject
//! diagnostic: positional field
module Conf;

public record Point(X: int, Y: int)
  public property X: int;
end Point;
