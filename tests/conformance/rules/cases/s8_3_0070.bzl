//! case: s8_3_0070
//! doc: spec
//! section: 8.3
//! title: an owning choice overriding Equals(Object?) without GetHashCode is rejected
//! rule: A type that overrides `Equals` must also override `GetHashCode`.
//!   The compiler enforces this (v0.338, §8.3).
//! cites: 8.3, 35.4
//! expect: reject
//! diagnostic: gethashcode
//! issue: 6078
module Conf;
class R public V: int := 0; destructor end destructor; end R;

choice Res<T> Ok(owned v: R), No;
  public method Equals(o: Res<T>): bool => true;
  public override method Equals(o: Object?): bool => true;
end Res;
class Program public static method Main(): int return 0; end Main; end Program;
