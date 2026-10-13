//! case: s8_3_0069
//! doc: spec
//! section: 8.3
//! title: an enum overriding Equals(Object?) needs GetHashCode even with an own-typed Equals
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   The compiler enforces this (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 6078
module Conf;
class R public V: int := 0; destructor end destructor; end R;

enum En A, B;
  public method Equals(other: En): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end En;
class Program public static method Main(): int return 0; end Main; end Program;
