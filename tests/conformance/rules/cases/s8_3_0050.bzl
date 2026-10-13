//! case: s8_3_0050
//! doc: spec
//! section: 8.3
//! title: a GetHashCode of the wrong signature does not discharge the obligation
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: reject
//! diagnostic: gethashcode
//! issue: 5956
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: R): bool => V = o.V;
  public override method GetHashCode(x: int): int => x;
end R;
class Program public static method Main(): int return 0; end Main; end Program;
