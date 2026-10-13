//! case: s8_3_0041
//! doc: spec
//! section: 8.3
//! title: an override own-typed Equals with a plain GetHashCode is accepted on a record
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: ok
//! issue: 5955
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public override method Equals(o: R): bool => V = o.V;
  public method GetHashCode(): int => V;
end R;
class Program public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end Program;
