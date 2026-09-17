//! case: s25_2_0005
//! doc: spec
//! section: 25.2
//! title: the four relational operators declared together are admitted
//! rule: Comparison operators must be overloaded as a complete group: `<`, `>`,
//!   `<=`, `>=` together.
//! cites: 25.2
//! expect: accept
module Conf;

public record Weight(Kg: double)
  public static method operator <(a: Weight, b: Weight): bool => a.Kg < b.Kg;
  public static method operator >(a: Weight, b: Weight): bool => a.Kg > b.Kg;
  public static method operator <=(a: Weight, b: Weight): bool => a.Kg <= b.Kg;
  public static method operator >=(a: Weight, b: Weight): bool => a.Kg >= b.Kg;
end Weight;
