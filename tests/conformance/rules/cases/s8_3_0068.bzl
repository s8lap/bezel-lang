//! case: s8_3_0068
//! doc: spec
//! section: 8.3
//! title: a record overriding Equals(Object?) needs GetHashCode even with an own-typed Equals
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   The compiler enforces this (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 6078
module Conf;
class R public V: int := 0; destructor end destructor; end R;

record Rec
  public readonly property Amount: int;
  public constructor(amount: int) Amount := amount; end constructor;
  public method Equals(other: Rec): bool => true;
  public method GetHashCode(): int => 1;
  public override method Equals(o: Object?): bool => true;
end Rec;
class Program public static method Main(): int return 0; end Main; end Program;
