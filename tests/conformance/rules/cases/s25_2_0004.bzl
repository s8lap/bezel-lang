//! case: s25_2_0004
//! doc: spec
//! section: 25.2
//! title: a lone relational overload is rejected as an incomplete group
//! rule: Comparison operators must be overloaded as a complete group: `<`, `>`,
//!   `<=`, `>=` together; `=` and `<>` together.
//! cites: 25.2
//! expect: reject
//! diagnostic: group
module Conf;

public record Weight(Kg: double)
  public static method operator <(a: Weight, b: Weight): bool => a.Kg < b.Kg;
end Weight;
