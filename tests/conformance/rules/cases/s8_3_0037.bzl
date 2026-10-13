//! case: s8_3_0037
//! doc: spec
//! section: 8.3
//! title: a generic record's plain own-typed Equals with no GetHashCode is rejected
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
record G<T> public property V: T; public constructor(v: T) V := v; end constructor;
  public method Equals(o: G<T>): bool => true;
end G;
class Program public static method Main(): int return 0; end Main; end Program;
