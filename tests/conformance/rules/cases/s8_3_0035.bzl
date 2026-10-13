//! case: s8_3_0035
//! doc: spec
//! section: 8.3
//! title: an enum's plain own-typed Equals with no GetHashCode is rejected
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
enum E A, B;
  public method Equals(o: E): bool => int(this) = int(o);
end E;
class Program public static method Main(): int return 0; end Main; end Program;
