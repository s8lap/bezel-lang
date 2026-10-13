//! case: s35_2_0042
//! doc: spec
//! section: 35.2
//! title: a choice overriding Equals(Object?) needs GetHashCode even with an own-typed Equals
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   The compiler enforces this (v0.338, §8.3).
//! cites: 8.3, 35.2
//! expect: reject
//! diagnostic: gethashcode
//! issue: 6078
module Conf;
class R public V: int := 0; destructor end destructor; end R;

choice Ch Ok(x: int), No;
  public method Equals(other: Ch): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end Ch;
class Program public static method Main(): int return 0; end Main; end Program;
