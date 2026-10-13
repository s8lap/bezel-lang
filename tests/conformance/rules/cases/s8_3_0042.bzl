//! case: s8_3_0042
//! doc: spec
//! section: 8.3
//! title: a plain Equals(Object?) on a record obliges no GetHashCode
//! rule: On a value kind, a plain `Equals` obliges `GetHashCode`, and a
//!   plain `GetHashCode` discharges the obligation (v0.338, §8.3).
//! cites: 8.3
//! expect: run
//! output: ok
//! issue: 5955
module Conf;
record R public property V: int; public constructor(v: int) V := v; end constructor;
  public method Equals(o: Object?): bool => true;
end R;
class Program public static method Main(): int Console.WriteLine("ok"); return 0; end Main; end Program;
