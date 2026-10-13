//! case: s8_3_0036
//! doc: spec
//! section: 8.3
//! title: a value-like choice's plain own-typed Equals with no GetHashCode is rejected
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3, 35.7
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
choice K One, Two;
  public method Equals(o: K): bool => true;
end K;
class Program public static method Main(): int return 0; end Main; end Program;
