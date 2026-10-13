//! case: s8_3_0034
//! doc: spec
//! section: 8.3
//! title: a record's plain own-typed Equals with no GetHashCode is rejected
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3, 9.2
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5955
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
